/*
 * sip.c - minimal SIP user agent with G.711 RTP
 *
 * Shared by actsd, actsc and utbd; the file is identical in all three
 * source trees.  See sip.h.
 *
 * Signalling uses the Sofia-SIP "nua" user agent (transactions,
 * retransmissions, digest authentication, registration refresh,
 * OPTIONS).  Sofia's own SDP engine is switched off: this module offers
 * and answers G.711 only and moves the audio over RTP itself.
 *
 * Everything runs in one thread: sip_poll() steps the Sofia event loop,
 * in which the RTP sockets are registered too.
 *
 * Works with Sofia-SIP 1.12 and 1.13.
 *
 * Copyright (C) 2026 Gabor Temesi <info@tgabor.eu>
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* Context pointer types handed back by Sofia callbacks */
#define NUA_MAGIC_T      void
#define NUA_HMAGIC_T     struct sip_call
#define SU_ROOT_MAGIC_T  void
#define SU_WAKEUP_ARG_T  struct sip_call

#include <sofia-sip/su.h>
#include <sofia-sip/su_wait.h>
#include <sofia-sip/su_log.h>
#include <sofia-sip/nua.h>
#include <sofia-sip/nua_tag.h>
#include <sofia-sip/sip_header.h>
#include <sofia-sip/sip_status.h>
#include <sofia-sip/msg_addr.h>
#include <sofia-sip/msg_header.h>

#include <spandsp.h>

#include "sip.h"

#define PT_PCMU     0
#define PT_PCMA     8
#define MAX_ALLOW   16

struct sip_call {
    int                 used;
    int                 outgoing;
    int                 answered;
    int                 auth_tries;
    nua_handle_t       *nh;
    int                 rtp_fd;
    int                 rtp_port;
    su_wait_t           wait;
    int                 wait_idx;
    struct sockaddr_in  remote;         /* RTP peer, sin_port 0 = unknown */
    int                 pt;             /* negotiated payload type, -1 = none */
    uint16_t            tx_seq;
    uint32_t            tx_ts, tx_ssrc;
    int                 tx_first;
    int                 rx_started;
    uint32_t            rx_ssrc, rx_next_ts;
    void               *user;
};

static su_root_t       *root;
static nua_t           *nua;
static nua_handle_t    *reg_nh;
static sip_handlers_t   H;
static sip_call_t       calls[SIP_MAX_CALLS];

static struct {
    char    registrar_host[256];
    int     registrar_port;
    char    username[64];
    char    password[64];
    char    local_ip[64];           /* bound and advertised address */
    char    public_ip[64];
    int     rtp_min, rtp_max;
    char    ua[64];
    int     allow_any;
    int     nallow;
    struct { uint32_t net, mask; } allow[MAX_ALLOW];
    int     registered;             /* -1 unknown, 0 failed, 1 ok */
    int     reg_auth_tries;
    int     shutdown_done;
} S;

/* ------------------------------------------------------------------ */
/*  Helpers                                                             */
/* ------------------------------------------------------------------ */
static void slog(const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    if (!H.log) return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    H.log(msg);
}

static int resolve_ipv4(const char *host, struct in_addr *out)
{
    struct addrinfo hints, *ai;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (getaddrinfo(host, NULL, &hints, &ai) != 0) return -1;
    *out = ((struct sockaddr_in *)ai->ai_addr)->sin_addr;
    freeaddrinfo(ai);
    return 0;
}

/* Local address used to reach a peer, from the routing table */
static void local_ip_for(const char *peer, char *out, size_t len)
{
    struct sockaddr_in r, l;
    socklen_t ll = sizeof(l);
    int fd;

    snprintf(out, len, "127.0.0.1");
    memset(&r, 0, sizeof(r));
    r.sin_family = AF_INET;
    r.sin_port   = htons(9);
    if (!peer || resolve_ipv4(peer, &r.sin_addr) < 0)
        inet_pton(AF_INET, "192.0.2.1", &r.sin_addr);  /* default route */
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&r, sizeof(r)) == 0 &&
        getsockname(fd, (struct sockaddr *)&l, &ll) == 0)
        inet_ntop(AF_INET, &l.sin_addr, out, len);
    if (fd >= 0) close(fd);
}

/* Printable ASCII only, for caller ID that ends up in logs */
static void clean_copy(char *dst, size_t len, const char *src)
{
    size_t i = 0;
    for (; src && src[i] && i < len - 1; i++)
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7f && src[i] != '"') ? src[i] : '?';
    dst[i] = '\0';
}

static int source_allowed(struct in_addr a)
{
    uint32_t ip = ntohl(a.s_addr);
    if (S.allow_any) return 1;
    for (int i = 0; i < S.nallow; i++)
        if ((ip & S.allow[i].mask) == S.allow[i].net) return 1;
    return 0;
}

static void parse_allow(const char *list)
{
    char buf[512], *tok, *save = NULL;
    snprintf(buf, sizeof(buf), "%s", list ? list : "");
    for (tok = strtok_r(buf, ", ", &save); tok; tok = strtok_r(NULL, ", ", &save)) {
        if (!strcmp(tok, "any")) { S.allow_any = 1; continue; }
        if (S.nallow == MAX_ALLOW) break;
        int bits = 32;
        char *slash = strchr(tok, '/');
        if (slash) { *slash = '\0'; bits = atoi(slash + 1); }
        struct in_addr a;
        if (inet_pton(AF_INET, tok, &a) != 1 || bits < 0 || bits > 32) {
            slog("WARN: ignoring invalid allow entry '%s'", tok);
            continue;
        }
        uint32_t mask = bits ? 0xffffffffu << (32 - bits) : 0;
        S.allow[S.nallow].net  = ntohl(a.s_addr) & mask;
        S.allow[S.nallow].mask = mask;
        S.nallow++;
    }
}

/* Source of the request being handled (the packet's IP address) */
static int request_source(struct in_addr *out, char *txt, size_t len)
{
    msg_t *m = nua_current_request(nua);
    su_addrinfo_t *ai = m ? msg_addrinfo(m) : NULL;

    if (!ai || !ai->ai_addr || ai->ai_family != AF_INET) return -1;
    *out = ((struct sockaddr_in *)ai->ai_addr)->sin_addr;
    inet_ntop(AF_INET, out, txt, len);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  RTP                                                                 */
/* ------------------------------------------------------------------ */
static void rtp_receive(sip_call_t *c)
{
    uint8_t pkt[2048];
    struct sockaddr_in from;
    socklen_t fl;

    for (;;) {
        fl = sizeof(from);
        ssize_t n = recvfrom(c->rtp_fd, pkt, sizeof(pkt), 0, (struct sockaddr *)&from, &fl);
        if (n < 0) return;
        if (!c->used) return;

        /* Only from the negotiated peer address, RTP version 2 */
        if (c->remote.sin_port == 0 || from.sin_addr.s_addr != c->remote.sin_addr.s_addr)
            continue;
        if (n < 12 || (pkt[0] >> 6) != 2) continue;

        int cc  = pkt[0] & 0x0f;
        int ext = pkt[0] & 0x10;
        int pt  = pkt[1] & 0x7f;
        uint32_t ts   = (uint32_t)pkt[4] << 24 | pkt[5] << 16 | pkt[6] << 8 | pkt[7];
        uint32_t ssrc = (uint32_t)pkt[8] << 24 | pkt[9] << 16 | pkt[10] << 8 | pkt[11];
        ssize_t off = 12 + 4 * cc;
        if (ext && off + 4 <= n)
            off += 4 + 4 * ((pkt[off + 2] << 8) | pkt[off + 3]);
        ssize_t len = n - off;
        if (pkt[0] & 0x20) len -= pkt[n - 1];          /* padding */
        if (pt != c->pt || len <= 0 || off > n) continue;

        /* Loss detection from the timestamp; late packets are dropped */
        if (!c->rx_started || ssrc != c->rx_ssrc) {
            c->rx_started = 1;
            c->rx_ssrc    = ssrc;
            c->rx_next_ts = ts;
        }
        int32_t gap = (int32_t)(ts - c->rx_next_ts);
        if (gap < 0) continue;
        if (gap > 0 && gap <= 8000 && H.on_lost) H.on_lost(c, gap);
        c->rx_next_ts = ts + (uint32_t)len;

        int16_t pcm[160];
        for (ssize_t i = 0; i < len; ) {
            int k = 0;
            for (; k < 160 && i < len; k++, i++)
                pcm[k] = c->pt == PT_PCMA ? alaw_to_linear(pkt[off + i])
                                          : ulaw_to_linear(pkt[off + i]);
            if (H.on_audio) H.on_audio(c, pcm, k);
            if (!c->used) return;
        }
    }
}

static int rtp_ready(su_root_magic_t *magic, su_wait_t *w, sip_call_t *c)
{
    (void)magic;
    su_wait_events(w, c->rtp_fd);
    rtp_receive(c);
    return 0;
}

int sip_send_audio(sip_call_t *c, const int16_t *pcm, int n)
{
    uint8_t pkt[12 + 480];

    if (!c || !c->used || c->remote.sin_port == 0 || c->pt < 0 || n > 480)
        return -1;

    pkt[0]  = 0x80;
    pkt[1]  = (uint8_t)(c->pt | (c->tx_first ? 0x80 : 0));
    pkt[2]  = c->tx_seq >> 8;  pkt[3]  = c->tx_seq & 0xff;
    pkt[4]  = c->tx_ts >> 24;  pkt[5]  = c->tx_ts >> 16;
    pkt[6]  = c->tx_ts >> 8;   pkt[7]  = c->tx_ts & 0xff;
    pkt[8]  = c->tx_ssrc >> 24; pkt[9] = c->tx_ssrc >> 16;
    pkt[10] = c->tx_ssrc >> 8; pkt[11] = c->tx_ssrc & 0xff;
    for (int i = 0; i < n; i++)
        pkt[12 + i] = c->pt == PT_PCMA ? linear_to_alaw(pcm[i]) : linear_to_ulaw(pcm[i]);

    c->tx_first = 0;
    c->tx_seq++;
    c->tx_ts += (uint32_t)n;
    return sendto(c->rtp_fd, pkt, 12 + n, 0,
                  (struct sockaddr *)&c->remote, sizeof(c->remote)) < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
/*  Calls                                                               */
/* ------------------------------------------------------------------ */
static sip_call_t *new_call(void)
{
    for (int i = 0; i < SIP_MAX_CALLS; i++) {
        if (calls[i].used) continue;
        sip_call_t *c = &calls[i];
        memset(c, 0, sizeof(*c));
        c->used     = 1;
        c->rtp_fd   = -1;
        c->wait_idx = -1;
        c->pt       = -1;
        c->tx_ssrc  = (uint32_t)random();
        c->tx_seq   = (uint16_t)random();
        c->tx_ts    = (uint32_t)random();
        c->tx_first = 1;

        /* RTP on an even port in the configured range */
        int span  = (S.rtp_max - S.rtp_min) / 2 + 1;
        int start = (int)(random() % span);
        for (int k = 0; k < span; k++) {
            int port = S.rtp_min + 2 * ((start + k) % span);
            int fd = socket(AF_INET, SOCK_DGRAM, 0);
            struct sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_port   = htons(port);
            if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                c->rtp_fd   = fd;
                c->rtp_port = port;
                su_wait_create(&c->wait, fd, SU_WAIT_IN);
                c->wait_idx = su_root_register(root, &c->wait, rtp_ready, c, 0);
                return c;
            }
            if (fd >= 0) close(fd);
        }
        slog("ERROR: no free RTP port in %d-%d", S.rtp_min, S.rtp_max);
        c->used = 0;
        return NULL;
    }
    slog("ERROR: too many calls");
    return NULL;
}

/* Release our side; the nua handle is destroyed when its dialog ends */
static void free_call(sip_call_t *c)
{
    if (c->wait_idx >= 0) su_root_deregister(root, c->wait_idx);
    if (c->rtp_fd >= 0) close(c->rtp_fd);
    if (c->nh) nua_handle_bind(c->nh, NULL);
    c->wait_idx = -1;
    c->rtp_fd   = -1;
    c->nh       = NULL;
    c->used     = 0;
}

static void end_call(sip_call_t *c, const char *why)
{
    free_call(c);
    if (H.on_ended) H.on_ended(c, why);
}

/* ------------------------------------------------------------------ */
/*  SDP                                                                 */
/* ------------------------------------------------------------------ */
static void build_sdp(sip_call_t *c, char *buf, size_t len)
{
    const char *ip = S.public_ip[0] ? S.public_ip : S.local_ip;

    int n = snprintf(buf, len,
        "v=0\r\n"
        "o=- %u %u IN IP4 %s\r\n"
        "s=%s\r\n"
        "c=IN IP4 %s\r\n"
        "t=0 0\r\n",
        c->tx_ssrc, (unsigned)time(NULL), ip, S.ua, ip);

    if (c->pt < 0)          /* offer both */
        snprintf(buf + n, len - n,
            "m=audio %d RTP/AVP 8 0\r\n"
            "a=rtpmap:8 PCMA/8000\r\n"
            "a=rtpmap:0 PCMU/8000\r\n"
            "a=ptime:20\r\n"
            "a=sendrecv\r\n", c->rtp_port);
    else
        snprintf(buf + n, len - n,
            "m=audio %d RTP/AVP %d\r\n"
            "a=rtpmap:%d %s/8000\r\n"
            "a=ptime:20\r\n"
            "a=sendrecv\r\n", c->rtp_port, c->pt, c->pt,
            c->pt == PT_PCMA ? "PCMA" : "PCMU");
}

static int has_sdp(sip_t const *sip)
{
    return sip && sip->sip_payload && sip->sip_payload->pl_len > 0;
}

/*
 * Take the remote address and codec from the SDP in a message.  Only
 * G.711 (PCMA or PCMU, the first one offered) is accepted.  Returns 0
 * on success, -1 if there is no usable audio stream.
 */
static int parse_sdp(sip_call_t *c, sip_t const *sip)
{
    char body[4096], ip[64] = "", line[256];
    int port = -1, pt = -1;

    if (!has_sdp(sip)) return -1;
    snprintf(body, sizeof(body), "%.*s",
             (int)(sip->sip_payload->pl_len < sizeof(body) - 1
                   ? sip->sip_payload->pl_len : sizeof(body) - 1),
             sip->sip_payload->pl_data);

    const char *p = body;
    while (*p) {
        size_t l = strcspn(p, "\r\n");
        snprintf(line, sizeof(line), "%.*s", (int)(l < sizeof(line) - 1 ? l : sizeof(line) - 1), p);
        p += l;
        while (*p == '\r' || *p == '\n') p++;

        if (!strncmp(line, "c=IN IP4 ", 9)) {
            snprintf(ip, sizeof(ip), "%.63s", line + 9);
            ip[strcspn(ip, " /")] = '\0';
        } else if (!strncmp(line, "m=audio ", 8) && port < 0) {
            char *s = line + 8, *end;
            port = (int)strtol(s, &end, 10);
            if (strncmp(end, " RTP/AVP", 8) != 0) { port = -1; continue; }
            for (s = end + 8; *s; ) {
                long v = strtol(s, &end, 10);
                if (end == s) break;
                if (pt < 0 && (v == PT_PCMA || v == PT_PCMU)) pt = (int)v;
                s = end;
            }
        }
    }

    struct in_addr a;
    if (port <= 0 || port > 65535 || pt < 0 || inet_pton(AF_INET, ip, &a) != 1 || a.s_addr == 0)
        return -1;

    /* keep the codec of an established call if it is still offered */
    if (c->pt >= 0 && pt != c->pt && strstr(body, c->pt == PT_PCMA ? " 8" : " 0"))
        pt = c->pt;
    c->pt = pt;
    c->remote.sin_family = AF_INET;
    c->remote.sin_addr   = a;
    c->remote.sin_port   = htons(port);
    c->rx_started        = 0;
    return 0;
}

static void answer_with_sdp(nua_handle_t *nh, sip_call_t *c)
{
    char sdp[1024];
    build_sdp(c, sdp, sizeof(sdp));
    nua_respond(nh, SIP_200_OK,
                SIPTAG_CONTENT_TYPE_STR("application/sdp"),
                SIPTAG_PAYLOAD_STR(sdp), TAG_END());
}

/* ------------------------------------------------------------------ */
/*  Digest authentication                                               */
/* ------------------------------------------------------------------ */
static int authenticate(nua_handle_t *nh, sip_t const *sip)
{
    msg_auth_t const *au = sip ? (sip->sip_www_authenticate
                                  ? sip->sip_www_authenticate
                                  : sip->sip_proxy_authenticate) : NULL;
    char cred[256];

    if (!au || !S.username[0]) return -1;
    char const *realm = msg_params_find(au->au_params, "realm=");
    snprintf(cred, sizeof(cred), "%s:%s:%s:%s", au->au_scheme,
             realm ? realm : "\"\"", S.username, S.password);
    nua_authenticate(nh, NUTAG_AUTH(cred), TAG_END());
    return 0;
}

/* ------------------------------------------------------------------ */
/*  SIP events                                                          */
/* ------------------------------------------------------------------ */
static void on_invite(nua_handle_t *nh, sip_call_t *c, sip_t const *sip)
{
    struct in_addr src;
    char src_txt[64] = "?", caller[128] = "unknown";

    if (c) {
        /* re-INVITE in an existing call */
        if (has_sdp(sip) && parse_sdp(c, sip) < 0) {
            nua_respond(nh, SIP_488_NOT_ACCEPTABLE, TAG_END());
            return;
        }
        answer_with_sdp(nh, c);
        return;
    }

    if (request_source(&src, src_txt, sizeof(src_txt)) < 0 || !source_allowed(src)) {
        static time_t last;
        if (time(NULL) != last) {
            last = time(NULL);
            slog("Rejected call from unauthorised source %s", src_txt);
        }
        nua_respond(nh, SIP_403_FORBIDDEN, TAG_END());
        return;
    }

    if (sip && sip->sip_from) {
        char name[48] = "", user[48] = "";
        clean_copy(name, sizeof(name), sip->sip_from->a_display);
        clean_copy(user, sizeof(user), sip->sip_from->a_url->url_user);
        snprintf(caller, sizeof(caller), "%s%s<%s>", name, name[0] ? " " : "",
                 user[0] ? user : "anonymous");
    }

    c = new_call();
    if (!c) {
        nua_respond(nh, SIP_503_SERVICE_UNAVAILABLE, TAG_END());
        return;
    }
    c->nh = nh;
    nua_handle_bind(nh, c);

    /* An INVITE without SDP is a late offer: we offer in the 200 OK */
    if (has_sdp(sip) && parse_sdp(c, sip) < 0) {
        slog("Rejected call from %s: no G.711 audio offered", caller);
        free_call(c);
        nua_respond(nh, SIP_488_NOT_ACCEPTABLE, TAG_END());
        return;
    }

    int status = H.on_incoming ? H.on_incoming(c, caller) : 486;
    if (status) {
        free_call(c);
        nua_respond(nh, status, sip_status_phrase(status), TAG_END());
        return;
    }
    answer_with_sdp(nh, c);
    c->answered = 1;
}

static void on_event(nua_event_t event, int status, char const *phrase,
                     nua_t *n, void *magic, nua_handle_t *nh, sip_call_t *c,
                     sip_t const *sip, tagi_t tags[])
{
    (void)n; (void)magic;
    char why[96];

    switch (event) {
        case nua_r_register:
            if (status == 401 || status == 407) {
                if (++S.reg_auth_tries <= 2 && authenticate(nh, sip) == 0)
                    break;
            }
            if (status == 200) {
                S.reg_auth_tries = 0;
                if (S.registered != 1)
                    slog("Registered with %s as %s", S.registrar_host, S.username);
                S.registered = 1;
            } else if (status >= 300 && S.registered != 0) {
                slog("ERROR: registration with %s failed: %d %s",
                     S.registrar_host, status, phrase);
                S.registered = 0;
            }
            break;

        case nua_i_invite:
            on_invite(nh, c, sip);
            break;

        case nua_i_ack:
            /* late offer: the SDP answer arrives in the ACK */
            if (c && c->remote.sin_port == 0)
                parse_sdp(c, sip);
            break;

        case nua_r_invite:
            if (!c || status < 200) break;
            if ((status == 401 || status == 407) && ++c->auth_tries <= 2 &&
                authenticate(nh, sip) == 0)
                break;
            if (status >= 300) {
                snprintf(why, sizeof(why), "SIP %d %s", status, phrase);
                end_call(c, why);
                break;
            }
            if (c->answered) break;             /* retransmitted 200 OK */
            if (parse_sdp(c, sip) < 0) {
                nua_bye(nh, TAG_END());
                end_call(c, "no G.711 audio in answer");
                break;
            }
            c->answered = 1;
            if (H.on_answered) H.on_answered(c);
            break;

        case nua_i_bye:
        case nua_i_cancel:
            if (c) end_call(c, "remote hangup");
            break;

        case nua_i_state: {
            int cs = nua_callstate_init;
            tl_gets(tags, NUTAG_CALLSTATE_REF(cs), TAG_END());
            if (cs == nua_callstate_terminated) {
                if (c) end_call(c, "call terminated");
                if (nh != reg_nh) nua_handle_destroy(nh);
            }
            break;
        }

        case nua_r_shutdown:
            if (status >= 200) S.shutdown_done = 1;
            break;

        default:
            break;
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                          */
/* ------------------------------------------------------------------ */
int sip_init(const sip_config_t *cfg, const sip_handlers_t *h)
{
    char url[160], proxy[300];

    memset(&S, 0, sizeof(S));
    S.registered = -1;
    H = *h;
    srandom((unsigned)time(NULL) ^ (unsigned)getpid());

    S.rtp_min = cfg->rtp_port_min & ~1;
    S.rtp_max = cfg->rtp_port_max;
    snprintf(S.username,  sizeof(S.username),  "%s", cfg->username  ? cfg->username  : "");
    snprintf(S.password,  sizeof(S.password),  "%s", cfg->password  ? cfg->password  : "");
    snprintf(S.public_ip, sizeof(S.public_ip), "%s", cfg->public_ip ? cfg->public_ip : "");
    snprintf(S.ua,        sizeof(S.ua),        "%s", cfg->user_agent ? cfg->user_agent : "sip");

    /* registrar: [sip:]host[:port] */
    if (cfg->registrar && cfg->registrar[0]) {
        const char *r = cfg->registrar;
        if (!strncmp(r, "sip:", 4)) r += 4;
        snprintf(S.registrar_host, sizeof(S.registrar_host), "%s", r);
        char *colon = strchr(S.registrar_host, ':');
        S.registrar_port = 5060;
        if (colon) { *colon = '\0'; S.registrar_port = atoi(colon + 1); }
    }

    parse_allow(cfg->allow);
    if (!S.allow_any && S.nallow == 0 && S.registrar_host[0]) {
        struct in_addr a;
        if (resolve_ipv4(S.registrar_host, &a) == 0) {
            S.allow[0].net  = ntohl(a.s_addr);
            S.allow[0].mask = 0xffffffffu;
            S.nallow = 1;
        } else {
            slog("WARN: cannot resolve registrar %s", S.registrar_host);
        }
    }

    su_init();
    su_log_set_level(NULL, 1);          /* Sofia's own messages: errors only */
    root = su_root_create(NULL);
    if (!root) {
        slog("ERROR: cannot create the SIP event loop");
        return -1;
    }

    /*
     * Bind one address.  With a wildcard Sofia opens a socket per
     * interface and may send from the wrong one on a multi-homed host,
     * so use the address that routes to the registrar (or the default
     * route) unless one is configured.  Behind NAT, advertise public_ip.
     */
    if (cfg->local_ip && cfg->local_ip[0])
        snprintf(S.local_ip, sizeof(S.local_ip), "%s", cfg->local_ip);
    else
        local_ip_for(S.registrar_host[0] ? S.registrar_host : NULL,
                     S.local_ip, sizeof(S.local_ip));
    if (S.public_ip[0])
        snprintf(url, sizeof(url), "sip:%s:%d;maddr=%s;transport=udp",
                 S.public_ip, cfg->port, S.local_ip);
    else
        snprintf(url, sizeof(url), "sip:%s:%d;transport=udp", S.local_ip, cfg->port);
    snprintf(proxy, sizeof(proxy), "sip:%s:%d", S.registrar_host, S.registrar_port);

    nua = nua_create(root, on_event, NULL,
                     NUTAG_URL(url),
                     TAG_IF(S.registrar_host[0], NUTAG_PROXY(proxy)),
                     TAG_IF(S.username[0], NUTAG_M_USERNAME(S.username)),
                     SIPTAG_USER_AGENT_STR(S.ua),
                     NUTAG_MEDIA_ENABLE(0),     /* SDP is handled here */
                     NUTAG_AUTOANSWER(0),
                     NUTAG_AUTOACK(1),
                     NUTAG_ALLOW("INVITE, ACK, BYE, CANCEL, OPTIONS, UPDATE, INFO"),
                     TAG_END());
    if (!nua) {
        slog("ERROR: cannot listen on SIP %s:%d/udp", S.local_ip, cfg->port);
        su_root_destroy(root);
        root = NULL;
        return -1;
    }

    if (cfg->do_register && S.registrar_host[0] && S.username[0]) {
        char aor[384];
        snprintf(aor, sizeof(aor), "sip:%s@%s", S.username, S.registrar_host);
        reg_nh = nua_handle(nua, NULL, SIPTAG_TO_STR(aor), SIPTAG_FROM_STR(aor), TAG_END());
        nua_register(reg_nh, NUTAG_REGISTRAR(proxy), SIPTAG_EXPIRES_STR("300"), TAG_END());
    }
    return 0;
}

void sip_poll(int timeout_ms)
{
    su_root_step(root, timeout_ms);
}

sip_call_t *sip_dial(const char *target)
{
    char to[384], from[384];

    /* "user" goes to the registrar; anything with '@' is a full URI */
    if (strchr(target, '@'))
        snprintf(to, sizeof(to), "%s%s", strncmp(target, "sip:", 4) ? "sip:" : "", target);
    else if (S.registrar_host[0])
        snprintf(to, sizeof(to), "sip:%s@%s:%d", target, S.registrar_host, S.registrar_port);
    else {
        slog("ERROR: '%s' needs a registrar or a full sip: URI", target);
        return NULL;
    }

    snprintf(from, sizeof(from), "sip:%s@%s", S.username[0] ? S.username : "anonymous",
             S.registrar_host[0] ? S.registrar_host
                                 : (S.public_ip[0] ? S.public_ip : S.local_ip));

    sip_call_t *c = new_call();
    if (!c) return NULL;
    c->outgoing = 1;

    c->nh = nua_handle(nua, c, SIPTAG_TO_STR(to), SIPTAG_FROM_STR(from), TAG_END());
    if (!c->nh) {
        slog("ERROR: invalid SIP address %s", to);
        free_call(c);
        return NULL;
    }

    char sdp[1024];
    build_sdp(c, sdp, sizeof(sdp));
    nua_invite(c->nh, SIPTAG_CONTENT_TYPE_STR("application/sdp"),
               SIPTAG_PAYLOAD_STR(sdp), TAG_END());
    slog("Calling %s", to);
    return c;
}

void sip_hangup(sip_call_t *c)
{
    if (!c || !c->used) return;
    if (c->nh) {
        if (c->answered)
            nua_bye(c->nh, TAG_END());
        else
            nua_cancel(c->nh, TAG_END());
    }
    free_call(c);
}

void sip_set_user(sip_call_t *c, void *user) { c->user = user; }
void *sip_get_user(const sip_call_t *c)      { return c->user; }

void sip_shutdown(void)
{
    if (!nua) return;
    for (int i = 0; i < SIP_MAX_CALLS; i++)
        if (calls[i].used) sip_hangup(&calls[i]);

    if (reg_nh) {
        nua_unregister(reg_nh, TAG_END());
        for (int i = 0; i < 25; i++) su_root_step(root, 20);
    }
    /* nua_shutdown waits for open transactions (BYE, unREGISTER) */
    nua_shutdown(nua);
    for (int i = 0; i < 200 && !S.shutdown_done; i++) su_root_step(root, 20);
    nua_destroy(nua);
    nua = NULL;
    reg_nh = NULL;
    su_root_destroy(root);
    root = NULL;
    su_deinit();
}

/*
 * sip.h - minimal SIP user agent with G.711 RTP
 *
 * Shared by actsd, actsc and utbd; the file is identical in all three
 * source trees.  Signalling uses Sofia-SIP.  RTP is handled here,
 * deliberately without jitter buffer, packet-loss concealment, silence
 * suppression or echo cancellation, so modem signals pass untouched.
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
#ifndef ACTS_SIPUA_H     /* not SIP_H: Sofia-SIP uses that */
#define ACTS_SIPUA_H

#include <stdint.h>

#define SIP_MAX_CALLS   64

typedef struct {
    int         port;           /* local SIP port (UDP) */
    const char *registrar;      /* "host[:port]" of the PBX / provider, or "" */
    int         do_register;    /* 1 = REGISTER with the registrar */
    const char *username;       /* our SIP user, also used for digest auth */
    const char *password;
    const char *local_ip;       /* local address for SIP and RTP, or "" for
                                   the one that routes to the registrar */
    const char *public_ip;      /* address to advertise behind NAT, or "" */
    int         rtp_port_min;
    int         rtp_port_max;
    const char *allow;          /* who may call in: "any", or a comma-separated
                                   list of IPv4 addresses / CIDR networks;
                                   "" = the registrar only */
    const char *user_agent;
} sip_config_t;

typedef struct sip_call sip_call_t;

typedef struct {
    /* Incoming call: return 0 to answer, or a SIP status code to reject */
    int  (*on_incoming)(sip_call_t *c, const char *caller);
    /* Outgoing call answered: media is flowing */
    void (*on_answered)(sip_call_t *c);
    /* Received audio, linear 16-bit 8 kHz, at most 160 samples at a time */
    void (*on_audio)(sip_call_t *c, const int16_t *pcm, int n);
    /* Received audio was lost: number of missing samples */
    void (*on_lost)(sip_call_t *c, int samples);
    /* The far end hung up or the call failed (not called for sip_hangup) */
    void (*on_ended)(sip_call_t *c, const char *why);
    /* Diagnostic messages */
    void (*log)(const char *msg);
} sip_handlers_t;

int         sip_init(const sip_config_t *cfg, const sip_handlers_t *h);
void        sip_shutdown(void);

/* Wait up to timeout_ms for RTP, then process SIP events */
void        sip_poll(int timeout_ms);

/* Place a call to "sip:user@host" or, with a registrar, just "user" */
sip_call_t *sip_dial(const char *target);

/* Send 8 kHz linear audio (normally 160 samples = 20 ms) */
int         sip_send_audio(sip_call_t *c, const int16_t *pcm, int n);

/* Hang up locally; the call handle is invalid afterwards */
void        sip_hangup(sip_call_t *c);

void        sip_set_user(sip_call_t *c, void *user);
void       *sip_get_user(const sip_call_t *c);

#endif

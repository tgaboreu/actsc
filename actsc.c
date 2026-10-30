/*
 * actsc.c - ACTS Time Service Client
 *
 * Connects to an ACTS-compatible time server (such as actsd), receives
 * and parses NIST-format UTC time strings, validates reliability across
 * multiple samples, and sets the system clock.
 *
 * Two modem modes are supported:
 *
 *   SIP mode  : Places a SIP call (directly, or through a PBX or
 *               provider) and runs a software V.22bis 2400bps modem
 *               with spandsp over G.711 RTP.  See sip.c.
 *
 *   TTY mode  : Drives a physical Hayes-compatible modem attached to a
 *               serial port (e.g. /dev/ttyUSB0).  Sends user-supplied
 *               AT initialisation commands, dials, then reads plain
 *               ASCII data from the modem's transparent data path.
 *
 * Two run modes:
 *
 *   one-shot  : Connect once, collect samples, set clock, exit.
 *               Default when run from the command line.
 *
 *   service   : Loop forever, sleeping poll_interval seconds between
 *               synchronisation attempts.  Intended for use with the
 *               supplied systemd unit.
 *
 * Each on-time marker ('*') is timestamped on arrival.  The clock is
 * stepped by the median offset of min_samples consecutive markers.
 * The client always hangs up first, so the modem is back in command
 * mode before the server drops its carrier.
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
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <getopt.h>
#include <limits.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <syslog.h>

#include <spandsp.h>

#include "sip.h"

/* ------------------------------------------------------------------ */
/*  Version                                                             */
/* ------------------------------------------------------------------ */
#define ACTSC_VERSION           "2.0.0"

/* ------------------------------------------------------------------ */
/*  Defaults                                                            */
/* ------------------------------------------------------------------ */
#define DEFAULT_CONFIG_FILE     "/etc/actsc/actsc.conf"
#define DEFAULT_SIP_PORT        5062
#define DEFAULT_RTP_MIN         10100
#define DEFAULT_RTP_MAX         10199
#define DEFAULT_POLL_INTERVAL   3600
#define DEFAULT_MIN_SAMPLES     10          /* valid OTMs before set    */
#define DEFAULT_MAX_OFFSET_SEC  300
#define DEFAULT_TTY_BAUD        B2400
#define DEFAULT_CALL_TIMEOUT    60
#define DEFAULT_DIAL_TIMEOUT    45

/* ------------------------------------------------------------------ */
/*  Log file paths                                                      */
/* ------------------------------------------------------------------ */
#define LOG_DIR                 "/var/log/actsc"
#define LOG_PATH_SYNC           LOG_DIR "/sync"
#define LOG_PATH_CONNECTIONS    LOG_DIR "/connections"
#define LOG_PATH_MESSAGES       LOG_DIR "/messages"

/* ------------------------------------------------------------------ */
/*  Modem / audio parameters (SIP mode)                                 */
/* ------------------------------------------------------------------ */
#define FRAME_SAMPLES           160
#define FRAME_MS                20
#define HANDSHAKE_TIMEOUT_SEC   45

/* ------------------------------------------------------------------ */
/*  Misc helpers                                                        */
/* ------------------------------------------------------------------ */
#define WRITE_DISCARD(fd, buf, len) \
do { ssize_t _wr = write((fd), (buf), (len)); (void)_wr; } while (0)

/* ------------------------------------------------------------------ */
/*  RX deframing state (SIP mode)                                       */
/* ------------------------------------------------------------------ */
typedef enum {
    RX_STATE_IDLE,
    RX_STATE_DATA,
    RX_STATE_STOP
} rx_state_t;

/* ------------------------------------------------------------------ */
/*  Call / modem state machine (SIP mode)                               */
/*                                                                      */
/*  IDLE       : No call in progress.                                   */
/*  CALLING    : INVITE sent, awaiting 200 OK.                          */
/*  HANDSHAKE  : Call answered, V.22bis training in progress.           */
/*  CONNECTED  : Our modem finished training; data bits are decoded.    */
/*  DONE       : Clock set successfully.  Hang up cleanly.              */
/*  HANGUP     : Error path hang-up.                                    */
/* ------------------------------------------------------------------ */
typedef enum {
    CSTATE_IDLE,
    CSTATE_CALLING,
    CSTATE_HANDSHAKE,
    CSTATE_CONNECTED,
    CSTATE_DONE,
    CSTATE_HANGUP
} call_state_t;

/*
 * TX_ACTIVE(s) — true while the V.22bis TX must keep running and
 * received audio must be fed to spandsp.  CALLING is excluded because
 * there is no media path before the call is answered.
 */
#define TX_ACTIVE(s) ((s) == CSTATE_HANDSHAKE || (s) == CSTATE_CONNECTED)

/* ------------------------------------------------------------------ */
/*  Modem mode                                                          */
/* ------------------------------------------------------------------ */
typedef enum {
    MODE_SIP,
    MODE_TTY
} modem_mode_t;

/* ------------------------------------------------------------------ */
/*  Time sample                                                         */
/* ------------------------------------------------------------------ */
typedef struct {
    time_t   acts_time;
    long     offset_ms;     /* system clock - ACTS time at the OTM */
} time_sample_t;

#define MAX_SAMPLES 16

/* ------------------------------------------------------------------ */
/*  Configuration                                                       */
/* ------------------------------------------------------------------ */
typedef struct {
    modem_mode_t mode;

    /* SIP */
    char registrar[256];
    char username[64];
    char password[64];
    char local_ip[64];
    char public_ip[64];
    int  sip_port;
    int  rtp_min, rtp_max;
    char number[128];           /* TTY: phone number; SIP: user or sip: URI */

    /* TTY */
    char tty_device[PATH_MAX];
    char at_init[256];
    char dial_prefix[32];
    int  tty_baud_flag;

    /* Common */
    int  poll_interval;
    int  min_samples;
    int  max_offset_sec;
    int  service_mode;
    int  dry_run;
    int  call_timeout;
    int  dial_timeout;

    char config_file[PATH_MAX];
    int  config_file_required;
} actsc_config_t;

static actsc_config_t g_cfg;

/* ------------------------------------------------------------------ */
/*  Global state                                                        */
/* ------------------------------------------------------------------ */
typedef struct {
    /* SIP */
    sip_call_t         *call;
    call_state_t        cstate;
    struct timespec     next_tx;
    time_t              call_start;

    /* spandsp V.22bis modem */
    v22bis_state_t     *modem;
    int                 carrier_count;

    /* RX deframing */
    rx_state_t          rx_char_state;
    int                 rx_char_bits;
    int                 rx_char_bit_count;

    unsigned long       frames_sent;
    unsigned long       frames_recv;

    /* TTY */
    int                 tty_fd;

    /* ACTS line accumulator */
    char                line_buf[256];
    int                 line_len;

    /* Time samples */
    time_sample_t       samples[MAX_SAMPLES];
    int                 sample_count;

    /* Sync result */
    int                 sync_done;
    int                 sync_attempted;

    /* Program lifecycle */
    int                 running;
} actsc_t;

static actsc_t g;

/* ------------------------------------------------------------------ */
/*  Log file handles                                                    */
/* ------------------------------------------------------------------ */
static FILE *log_f_sync = NULL;
static FILE *log_f_conn = NULL;
static FILE *log_f_msg  = NULL;

/* ------------------------------------------------------------------ */
/*  Logging                                                             */
/*                                                                      */
/*  LOG()       - general: stdout (the journal under systemd)          */
/*  LOG_SYNC()  - clock synchronisation: file + syslog                 */
/*  LOG_CONN()  - connection / handshake: file + syslog                */
/*  LOG_MSG()   - received messages: file + syslog                     */
/* ------------------------------------------------------------------ */
#define LOG(fmt, ...) \
do { \
    time_t _t = time(NULL); \
    struct tm *_tm = gmtime(&_t); \
    fprintf(stdout, "[%04d-%02d-%02d %02d:%02d:%02dZ] " fmt "\n", \
            _tm->tm_year+1900, _tm->tm_mon+1, _tm->tm_mday, \
            _tm->tm_hour, _tm->tm_min, _tm->tm_sec, ##__VA_ARGS__); \
    fflush(stdout); \
} while(0)

static void log_write(FILE *f, int prio,
                      const char *category, const char *fmt, ...)
{
    va_list ap;
    char    msg[1024];
    char    ts[32];
    time_t  now = time(NULL);
    struct tm *tm = gmtime(&now);

    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S UTC", tm);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (f) {
        fprintf(f, "[%s] %s\n", ts, msg);
        fflush(f);
    }

    syslog(prio, "[%s] %s", category, msg);
}

#define LOG_SYNC(prio, ...) log_write(log_f_sync, prio, "SYNC",       __VA_ARGS__)
#define LOG_CONN(prio, ...) log_write(log_f_conn, prio, "CONNECTION", __VA_ARGS__)
#define LOG_MSG(prio, ...)  log_write(log_f_msg,  prio, "MESSAGE",    __VA_ARGS__)

/* ------------------------------------------------------------------ */
/*  Log file management                                                 */
/* ------------------------------------------------------------------ */
static int logs_open(void)
{
    if (mkdir(LOG_DIR, 0750) < 0 && errno != EEXIST) {
        fprintf(stderr, "ERROR: Cannot create log directory %s: %s\n",
                LOG_DIR, strerror(errno));
        return -1;
    }

    log_f_sync = fopen(LOG_PATH_SYNC,        "a");
    log_f_conn = fopen(LOG_PATH_CONNECTIONS, "a");
    log_f_msg  = fopen(LOG_PATH_MESSAGES,    "a");

    if (!log_f_sync || !log_f_conn || !log_f_msg) {
        fprintf(stderr, "ERROR: Cannot open log files in %s: %s\n",
                LOG_DIR, strerror(errno));
        return -1;
    }

    return 0;
}

static void logs_close(void)
{
    if (log_f_sync) { fclose(log_f_sync); log_f_sync = NULL; }
    if (log_f_conn) { fclose(log_f_conn); log_f_conn = NULL; }
    if (log_f_msg)  { fclose(log_f_msg);  log_f_msg  = NULL; }
}

static void logs_reopen(void)
{
    logs_close();
    logs_open();
    LOG("Log files reopened (SIGHUP)");
}

/* ------------------------------------------------------------------ */
/*  Signal handling                                                     */
/* ------------------------------------------------------------------ */
static volatile sig_atomic_t g_got_sighup  = 0;
static volatile sig_atomic_t g_got_sigterm = 0;

static void sig_handler(int sig)
{
    if (sig == SIGHUP)
        g_got_sighup = 1;
    else
        g_got_sigterm = 1;
}

/* ------------------------------------------------------------------ */
/*  Config file parser                                                  */
/*                                                                      */
/*  Runs between two passes of the CLI parser, so values set here are  */
/*  overridden by any command-line option.                              */
/* ------------------------------------------------------------------ */
static int baud_from_string(const char *s)
{
    int n = atoi(s);
    switch (n) {
        case 1200:   return B1200;
        case 2400:   return B2400;
        case 4800:   return B4800;
        case 9600:   return B9600;
        case 19200:  return B19200;
        case 38400:  return B38400;
        case 57600:  return B57600;
        case 115200: return B115200;
        default:
            fprintf(stderr,
                    "WARNING: unknown baud rate '%s', using 2400\n", s);
            return B2400;
    }
}

static int parse_bool(const char *val)
{
    return atoi(val) != 0 ||
           strcmp(val, "yes")  == 0 ||
           strcmp(val, "true") == 0;
}

static int config_parse(actsc_config_t *cfg)
{
    FILE *f = fopen(cfg->config_file, "r");
    if (!f) {
        if (cfg->config_file_required) {
            fprintf(stderr, "ERROR: Cannot open config file %s: %s\n",
                    cfg->config_file, strerror(errno));
            return -1;
        }
        return 0;
    }

    char line[512];
    int  lineno = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;

        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;

        char *eq = strchr(p, '=');
        if (!eq) {
            fprintf(stderr, "WARNING: %s:%d: malformed line: %s\n",
                    cfg->config_file, lineno, line);
            continue;
        }

        char key[64] = {0};
        *eq = '\0';
        char *kend = eq - 1;
        while (kend > p && (*kend == ' ' || *kend == '\t')) kend--;
        snprintf(key, sizeof(key), "%.*s", (int)(kend - p + 1), p);

        char val[512] = {0};
        char *vp = eq + 1;
        while (*vp == ' ' || *vp == '\t') vp++;
        char *vend = vp + strlen(vp) - 1;
        while (vend > vp &&
               (*vend == ' ' || *vend == '\t' || *vend == '\r'))
            vend--;
        if (vp <= vend)
            snprintf(val, sizeof(val), "%.*s", (int)(vend - vp + 1), vp);

        if (strcmp(key, "mode") == 0) {
            if      (strcmp(val, "tty") == 0) cfg->mode = MODE_TTY;
            else if (strcmp(val, "sip") == 0) cfg->mode = MODE_SIP;
            else {
                fprintf(stderr, "ERROR: %s:%d: unknown mode '%s' (use sip or tty%s)\n",
                        cfg->config_file, lineno, val,
                        strcmp(val, "iax") ? "" : "; IAX2 was replaced by SIP in 2.0");
                fclose(f);
                return -1;
            }
        } else if (strcmp(key, "registrar") == 0) {
            snprintf(cfg->registrar, sizeof(cfg->registrar), "%s", val);
        } else if (strcmp(key, "username") == 0) {
            snprintf(cfg->username, sizeof(cfg->username), "%s", val);
        } else if (strcmp(key, "password") == 0) {
            snprintf(cfg->password, sizeof(cfg->password), "%s", val);
        } else if (strcmp(key, "local_ip") == 0) {
            snprintf(cfg->local_ip, sizeof(cfg->local_ip), "%s", val);
        } else if (strcmp(key, "public_ip") == 0) {
            snprintf(cfg->public_ip, sizeof(cfg->public_ip), "%s", val);
        } else if (strcmp(key, "sip_port") == 0) {
            cfg->sip_port = atoi(val);
        } else if (strcmp(key, "rtp_ports") == 0) {
            if (sscanf(val, "%d-%d", &cfg->rtp_min, &cfg->rtp_max) != 2)
                fprintf(stderr, "WARNING: %s:%d: rtp_ports must be MIN-MAX\n",
                        cfg->config_file, lineno);
        } else if (!strcmp(key, "host") || !strcmp(key, "port") ||
                   !strcmp(key, "peername") || !strcmp(key, "secret")) {
            fprintf(stderr, "ERROR: %s:%d: '%s' is an IAX2 setting; actsc 2.x "
                    "uses SIP (see README.md, 'Upgrading from 1.x')\n",
                    cfg->config_file, lineno, key);
            fclose(f);
            return -1;
        } else if (strcmp(key, "number") == 0) {
            snprintf(cfg->number, sizeof(cfg->number), "%s", val);
        } else if (strcmp(key, "tty") == 0) {
            snprintf(cfg->tty_device, sizeof(cfg->tty_device), "%s", val);
        } else if (strcmp(key, "at_init") == 0) {
            snprintf(cfg->at_init, sizeof(cfg->at_init), "%s", val);
        } else if (strcmp(key, "dial_prefix") == 0) {
            snprintf(cfg->dial_prefix, sizeof(cfg->dial_prefix), "%s", val);
        } else if (strcmp(key, "tty_baud") == 0) {
            cfg->tty_baud_flag = baud_from_string(val);
        } else if (strcmp(key, "poll_interval") == 0) {
            cfg->poll_interval = atoi(val);
        } else if (strcmp(key, "min_samples") == 0) {
            cfg->min_samples = atoi(val);
        } else if (strcmp(key, "max_offset_sec") == 0) {
            cfg->max_offset_sec = atoi(val);
        } else if (strcmp(key, "service_mode") == 0) {
            cfg->service_mode = parse_bool(val);
        } else if (strcmp(key, "dry_run") == 0) {
            cfg->dry_run = parse_bool(val);
        } else if (strcmp(key, "call_timeout") == 0) {
            cfg->call_timeout = atoi(val);
        } else if (strcmp(key, "dial_timeout") == 0) {
            cfg->dial_timeout = atoi(val);
        } else {
            fprintf(stderr, "WARNING: %s:%d: unknown key: %s\n",
                    cfg->config_file, lineno, key);
        }
    }

    fclose(f);
    return 0;
}

static void config_apply_defaults(actsc_config_t *cfg)
{
    if (cfg->sip_port       == 0) cfg->sip_port       = DEFAULT_SIP_PORT;
    if (cfg->rtp_min        == 0) { cfg->rtp_min = DEFAULT_RTP_MIN; cfg->rtp_max = DEFAULT_RTP_MAX; }
    if (cfg->poll_interval  == 0) cfg->poll_interval  = DEFAULT_POLL_INTERVAL;
    if (cfg->min_samples    == 0) cfg->min_samples    = DEFAULT_MIN_SAMPLES;
    if (cfg->max_offset_sec == 0) cfg->max_offset_sec = DEFAULT_MAX_OFFSET_SEC;
    if (cfg->tty_baud_flag  == 0) cfg->tty_baud_flag  = DEFAULT_TTY_BAUD;
    if (cfg->call_timeout   == 0) cfg->call_timeout   = DEFAULT_CALL_TIMEOUT;
    if (cfg->dial_timeout   == 0) cfg->dial_timeout   = DEFAULT_DIAL_TIMEOUT;
    if (cfg->dial_prefix[0] == '\0')
        snprintf(cfg->dial_prefix, sizeof(cfg->dial_prefix), "%s", "ATDT");
}

/* ------------------------------------------------------------------ */
/*  Config validation                                                   */
/* ------------------------------------------------------------------ */
static int config_validate(const actsc_config_t *cfg)
{
    int ok = 1;

    if (cfg->mode == MODE_SIP) {
        if (cfg->number[0] == '\0')
            { fprintf(stderr, "ERROR: 'number' required for SIP mode.\n");   ok = 0; }
        else if (!strchr(cfg->number, '@') && cfg->registrar[0] == '\0')
            { fprintf(stderr, "ERROR: 'number' must be a sip:user@host URI "
                              "when no 'registrar' is set.\n");             ok = 0; }
        if (cfg->sip_port <= 0 || cfg->sip_port > 65535)
            { fprintf(stderr, "ERROR: 'sip_port' must be 1..65535.\n");      ok = 0; }
        if (cfg->rtp_min < 1024 || cfg->rtp_max > 65535 || cfg->rtp_max <= cfg->rtp_min)
            { fprintf(stderr, "ERROR: 'rtp_ports' must be a range within 1024-65535.\n"); ok = 0; }
    } else {
        if (cfg->tty_device[0] == '\0')
            { fprintf(stderr, "ERROR: 'tty' required for TTY mode.\n");      ok = 0; }
        if (cfg->number[0]     == '\0')
            { fprintf(stderr, "ERROR: 'number' required for TTY mode.\n");   ok = 0; }
    }

    if (cfg->min_samples < 1 || cfg->min_samples > MAX_SAMPLES) {
        fprintf(stderr, "ERROR: 'min_samples' must be 1..%d.\n",
                MAX_SAMPLES);
        ok = 0;
    }

    if (cfg->poll_interval < 60) {
        fprintf(stderr, "ERROR: 'poll_interval' must be >= 60 seconds.\n");
        ok = 0;
    }

    return ok ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  Help and version                                                    */
/* ------------------------------------------------------------------ */
static void print_help(const char *prog)
{
    printf(
        "Usage: %s [OPTIONS]\n"
        "\n"
        "ACTS Time Service Client — synchronises the system clock via\n"
        "a NIST-compatible ACTS time service over V.22bis 2400bps.\n"
        "\n"
        "Options:\n"
        "  -m, --mode=MODE          sip or tty (default: sip)\n"
        "  -r, --registrar=HOST     SIP PBX / provider (SIP mode)\n"
        "  -u, --username=USER      SIP user name\n"
        "  -w, --password=PASS      SIP password\n"
        "  -p, --sip-port=PORT      Local SIP port (default: %d)\n"
        "  -N, --number=NUMBER      Number to dial; SIP: user or sip:user@host\n"
        "  -t, --tty=DEVICE         Serial device (TTY mode)\n"
        "  -A, --at-init=CMDS       AT init commands (comma-separated)\n"
        "  -P, --dial-prefix=PFX    Dial prefix (default: ATDT)\n"
        "  -B, --baud=RATE          Serial baud rate (default: 2400)\n"
        "  -c, --config=FILE        Config file (default: %s)\n"
        "  -S, --service            Service/daemon mode\n"
        "  -i, --interval=SECS      Poll interval (default: %d)\n"
        "      --min-samples=N      Valid OTMs before set (default: %d,"
        " max: %d)\n"
        "      --max-offset=SECS    Sanity ceiling (default: %d)\n"
        "      --call-timeout=SECS  Call timeout (default: %d)\n"
        "      --dry-run            Parse but do not set clock\n"
        "  -h, --help               This help\n"
        "  -V, --version            Version\n"
        "\n",
        prog,
        DEFAULT_SIP_PORT,
        DEFAULT_CONFIG_FILE,
        DEFAULT_POLL_INTERVAL,
        DEFAULT_MIN_SAMPLES, MAX_SAMPLES,
        DEFAULT_MAX_OFFSET_SEC,
        DEFAULT_CALL_TIMEOUT
    );
}

static void print_version(void)
{
    printf(
        "actsc %s\n"
        "Copyright (C) 2026 Gabor Temesi <info@tgabor.eu>\n"
        "License GPLv3+: GNU GPL version 3 or later <https://gnu.org/licenses/gpl.html>\n"
        "This is free software: you are free to change and redistribute it.\n"
        "There is NO WARRANTY, to the extent permitted by law.\n"
        "\n"
        "Libraries:\n"
        "  spandsp    (LGPL-2.1)          https://github.com/freeswitch/spandsp\n"
        "  Sofia-SIP  (LGPL-2.1-or-later) https://github.com/freeswitch/sofia-sip\n",
        ACTSC_VERSION
    );
}

/* ------------------------------------------------------------------ */
/*  CLI parser                                                          */
/* ------------------------------------------------------------------ */
enum {
    OPT_MIN_SAMPLES = 256,
    OPT_MAX_OFFSET,
    OPT_CALL_TIMEOUT,
    OPT_DRY_RUN
};

static int cli_parse(int argc, char *argv[], actsc_config_t *cfg)
{
    static const struct option long_opts[] = {
        { "mode",         required_argument, NULL, 'm' },
        { "registrar",    required_argument, NULL, 'r' },
        { "username",     required_argument, NULL, 'u' },
        { "password",     required_argument, NULL, 'w' },
        { "sip-port",     required_argument, NULL, 'p' },
        { "number",       required_argument, NULL, 'N' },
        { "tty",          required_argument, NULL, 't' },
        { "at-init",      required_argument, NULL, 'A' },
        { "dial-prefix",  required_argument, NULL, 'P' },
        { "baud",         required_argument, NULL, 'B' },
        { "config",       required_argument, NULL, 'c' },
        { "service",      no_argument,       NULL, 'S' },
        { "interval",     required_argument, NULL, 'i' },
        { "min-samples",  required_argument, NULL, OPT_MIN_SAMPLES },
        { "max-offset",   required_argument, NULL, OPT_MAX_OFFSET  },
        { "call-timeout", required_argument, NULL, OPT_CALL_TIMEOUT },
        { "dry-run",      no_argument,       NULL, OPT_DRY_RUN     },
        { "help",         no_argument,       NULL, 'h' },
        { "version",      no_argument,       NULL, 'V' },
        { NULL, 0, NULL, 0 }
    };

    int opt;
    while ((opt = getopt_long(argc, argv,
                              "m:r:u:w:p:N:t:A:P:B:c:Si:hV",
                              long_opts, NULL)) != -1) {
        switch (opt) {
            case 'm':
                if      (strcmp(optarg, "sip") == 0) cfg->mode = MODE_SIP;
                else if (strcmp(optarg, "tty") == 0) cfg->mode = MODE_TTY;
                else {
                    fprintf(stderr,
                            "ERROR: Unknown mode '%s' (use sip or tty)\n",
                            optarg);
                    return -1;
                }
                break;
            case 'r':
                snprintf(cfg->registrar, sizeof(cfg->registrar), "%s", optarg);
                break;
            case 'u':
                snprintf(cfg->username, sizeof(cfg->username), "%s", optarg);
                break;
            case 'w':
                snprintf(cfg->password, sizeof(cfg->password), "%s", optarg);
                break;
            case 'p':
                cfg->sip_port = atoi(optarg);
                break;
            case 'N':
                snprintf(cfg->number,   sizeof(cfg->number),   "%s", optarg);
                break;
            case 't':
                snprintf(cfg->tty_device, sizeof(cfg->tty_device),
                         "%s", optarg);
                break;
            case 'A':
                snprintf(cfg->at_init,  sizeof(cfg->at_init),  "%s", optarg);
                break;
            case 'P':
                snprintf(cfg->dial_prefix, sizeof(cfg->dial_prefix),
                         "%s", optarg);
                break;
            case 'B':
                cfg->tty_baud_flag = baud_from_string(optarg);
                break;
            case 'c':
                snprintf(cfg->config_file, sizeof(cfg->config_file),
                         "%s", optarg);
                cfg->config_file_required = 1;
                break;
            case 'S':
                cfg->service_mode = 1;
                break;
            case 'i':
                cfg->poll_interval = atoi(optarg);
                break;
            case OPT_MIN_SAMPLES:
                cfg->min_samples    = atoi(optarg); break;
            case OPT_MAX_OFFSET:
                cfg->max_offset_sec = atoi(optarg); break;
            case OPT_CALL_TIMEOUT:
                cfg->call_timeout   = atoi(optarg); break;
            case OPT_DRY_RUN:
                cfg->dry_run = 1; break;
            case 'h':
                print_help(argv[0]); exit(EXIT_SUCCESS);
            case 'V':
                print_version();     exit(EXIT_SUCCESS);
            default:
                fprintf(stderr, "Try '%s --help'.\n", argv[0]);
                return -1;
        }
    }

    if (optind < argc) {
        fprintf(stderr, "ERROR: Unexpected argument: %s\n", argv[optind]);
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Timing helpers                                                      */
/* ------------------------------------------------------------------ */
static void ts_add_ms(struct timespec *ts, long ms)
{
    ts->tv_nsec += ms * 1000000L;
    while (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static long ms_until(const struct timespec *deadline)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (deadline->tv_sec  - now.tv_sec)  * 1000L
         + (deadline->tv_nsec - now.tv_nsec) / 1000000L;
}

/* ------------------------------------------------------------------ */
/*  ACTS string parser                                                  */
/*                                                                      */
/*  Format: "61283 26-08-31 15:02:38 50 0 +.0 050.0 UTC(ACTS) "         */
/*  (the line up to, but not including, the on-time marker)             */
/* ------------------------------------------------------------------ */
static int acts_parse(const char *line, time_t *out_time)
{
    long mjd;
    int  yr, mo, da, hh, mm, ss, dst, ls;

    int n = sscanf(line,
                   " %ld %d-%d-%d %d:%d:%d %d %d %*s %*s UTC(ACTS)",
                   &mjd, &yr, &mo, &da, &hh, &mm, &ss, &dst, &ls);

    if (n < 9) return -1;

    struct tm utc;
    memset(&utc, 0, sizeof(utc));
    utc.tm_year = yr + 100;
    utc.tm_mon  = mo - 1;
    utc.tm_mday = da;
    utc.tm_hour = hh;
    utc.tm_min  = mm;
    utc.tm_sec  = ss;

    time_t t = timegm(&utc);
    if (t == (time_t)-1) return -1;

    *out_time = t;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  On-time marker received — called the moment '*' arrives            */
/* ------------------------------------------------------------------ */
static void on_otm(const char *line)
{
    struct timespec wall;
    clock_gettime(CLOCK_REALTIME, &wall);

    LOG_MSG(LOG_INFO, "RX: %s*", line);

    time_t acts_time;
    if (acts_parse(line, &acts_time) < 0) {
        LOG("WARN: Failed to parse ACTS string: '%s'", line);
        return;
    }

    /* Only consecutive seconds are trusted; a gap starts a new series */
    if (g.sample_count > 0 &&
        acts_time != g.samples[g.sample_count - 1].acts_time + 1) {
        LOG("Non-consecutive OTM (previous %ld, now %ld) - "
            "restarting sample series",
            (long)g.samples[g.sample_count - 1].acts_time, (long)acts_time);
        g.sample_count = 0;
    }

    if (g.sample_count == MAX_SAMPLES) {
        memmove(&g.samples[0], &g.samples[1],
                (MAX_SAMPLES - 1) * sizeof(time_sample_t));
        g.sample_count--;
    }

    long offset_ms = (long)(wall.tv_sec - acts_time) * 1000L
                   + wall.tv_nsec / 1000000L;

    g.samples[g.sample_count].acts_time = acts_time;
    g.samples[g.sample_count].offset_ms = offset_ms;
    g.sample_count++;

    LOG("OTM sample #%d: acts=%ld offset=%+ldms",
        g.sample_count, (long)acts_time, offset_ms);
    LOG_SYNC(LOG_INFO, "Sample #%d: acts_utc=%ld offset=%+ldms",
             g.sample_count, (long)acts_time, offset_ms);
}

/* ------------------------------------------------------------------ */
/*  Character accumulator — shared between SIP and TTY paths            */
/* ------------------------------------------------------------------ */
static void feed_char(uint8_t c)
{
    if (c == '*' && strstr(g.line_buf, "UTC(ACTS)")) {
        on_otm(g.line_buf);
    } else if (c >= 0x20 && c < 0x7f) {
        if (g.line_len < (int)sizeof(g.line_buf) - 1) {
            g.line_buf[g.line_len++] = (char)c;
            g.line_buf[g.line_len]   = '\0';
        }
    } else if (c == '\n') {
        if (g.line_len > 0 && !strstr(g.line_buf, "UTC(ACTS)"))
            LOG_MSG(LOG_INFO, "INFO: %s", g.line_buf);
        g.line_len    = 0;
        g.line_buf[0] = '\0';
    }
    /* '\r' and other control characters silently dropped */
}

/* ------------------------------------------------------------------ */
/*  Clock correction                                                    */
/*                                                                      */
/*  Returns 0 when the clock was set (or would be, in dry-run mode),   */
/*  -1 when it was refused or failed.                                   */
/* ------------------------------------------------------------------ */
static int compare_long(const void *a, const void *b)
{
    long la = *(const long *)a;
    long lb = *(const long *)b;
    return (la > lb) - (la < lb);
}

static int try_set_clock(void)
{
    long offsets[MAX_SAMPLES];
    int  n = g.sample_count;
    for (int i = 0; i < n; i++)
        offsets[i] = g.samples[i].offset_ms;
    qsort(offsets, n, sizeof(long), compare_long);
    long median_ms = offsets[n / 2];

    LOG("Clock analysis: samples=%d median_offset=%+ldms", n, median_ms);

    if (labs(median_ms) > (long)g_cfg.max_offset_sec * 1000L) {
        LOG("ERROR: Offset %+ldms exceeds sanity limit %ds — refusing",
            median_ms, g_cfg.max_offset_sec);
        LOG_SYNC(LOG_ERR,
                 "Clock set refused: offset=%+ldms limit=%ds",
                 median_ms, g_cfg.max_offset_sec);
        return -1;
    }

    if (g_cfg.dry_run) {
        LOG("[DRY-RUN] Would step clock by %+ldms", -median_ms);
        LOG_SYNC(LOG_NOTICE,
                 "[DRY-RUN] Would step clock by %+ldms (samples=%d)",
                 -median_ms, n);
        g.sync_done = 1;
        return 0;
    }

    /* offset = system clock - ACTS time, so step the clock back by it */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long long ns = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec
                 - (long long)median_ms * 1000000LL;
    ts.tv_sec  = (time_t)(ns / 1000000000LL);
    ts.tv_nsec = (long)(ns % 1000000000LL);

    if (clock_settime(CLOCK_REALTIME, &ts) < 0) {
        LOG("ERROR: clock_settime() failed: %s", strerror(errno));
        LOG_SYNC(LOG_ERR, "clock_settime() failed: %s", strerror(errno));
        return -1;
    }

    LOG("Clock stepped by %+ldms (samples=%d)", -median_ms, n);
    LOG_SYNC(LOG_NOTICE, "Clock stepped by %+ldms (samples=%d)",
             -median_ms, n);

    g.sync_done = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  SIP mode — spandsp V.22bis modem callbacks                          */
/* ------------------------------------------------------------------ */

/*
 * modem_get_bit — TX path.
 * actsc does not send data — return mark (1) = idle line.
 */
static int modem_get_bit(void *user_data)
{
    (void)user_data;
    return 1;
}

/*
 * modem_put_bit — RX path: status events + async 8N1 deframing.
 *
 *   HANDSHAKE --(TRAINING_SUCCEEDED)--> CONNECTED --(CARRIER_DOWN)--> HANGUP
 *
 * TRAINING_SUCCEEDED is reported by our own transmitter when it has
 * finished the training sequence; the receiver is trained by then.
 */
static void modem_put_bit(void *user_data, int bit)
{
    (void)user_data;

    if (bit < 0) {
        switch (bit) {
            case SIG_STATUS_CARRIER_UP:
                g.carrier_count++;
                LOG("V.22bis: CARRIER UP (event #%d)", g.carrier_count);
                break;

            case SIG_STATUS_TRAINING_SUCCEEDED:
                if (g.cstate != CSTATE_HANDSHAKE) break;
                LOG("V.22bis: TRAINING SUCCEEDED — receiving data");
                LOG_CONN(LOG_NOTICE, "V.22bis link established");
                g.cstate        = CSTATE_CONNECTED;
                g.rx_char_state = RX_STATE_IDLE;
                break;

            case SIG_STATUS_TRAINING_FAILED:
                LOG("V.22bis: TRAINING FAILED");
                break;

            case SIG_STATUS_CARRIER_DOWN:
                /* spandsp restarts the modem; after training this
                   means the server has ended the call */
                LOG("V.22bis: CARRIER DOWN (cstate=%d)", g.cstate);
                if (g.cstate == CSTATE_CONNECTED)
                    g.cstate = CSTATE_HANGUP;
                break;

            case SIG_STATUS_MODEM_RETRAIN_OCCURRED:
                LOG("V.22bis: RETRAIN");
                if (g.cstate == CSTATE_CONNECTED)
                    g.cstate = CSTATE_HANDSHAKE;
                break;

            default:
                break;
        }
        return;
    }

    if (g.cstate != CSTATE_CONNECTED) return;

    /* Async 8N1 deframing */
    switch (g.rx_char_state) {

        case RX_STATE_IDLE:
            if (bit == 0) {
                g.rx_char_state     = RX_STATE_DATA;
                g.rx_char_bits      = 0;
                g.rx_char_bit_count = 0;
            }
            break;

        case RX_STATE_DATA:
            g.rx_char_bits |= (bit & 1) << g.rx_char_bit_count;
            g.rx_char_bit_count++;
            if (g.rx_char_bit_count == 8)
                g.rx_char_state = RX_STATE_STOP;
            break;

        case RX_STATE_STOP:
            if (bit == 1)
                feed_char((uint8_t)g.rx_char_bits);
            else
                LOG("RX framing error: no stop bit (char=0x%02x)",
                    g.rx_char_bits);
            g.rx_char_state = RX_STATE_IDLE;
            break;
    }
}

/* ------------------------------------------------------------------ */
/*  SIP mode — modem init / destroy                                     */
/* ------------------------------------------------------------------ */
static int modem_init(void)
{
    if (g.modem) {
        v22bis_free(g.modem);
        g.modem = NULL;
    }

    /*
     * calling_party=1 : actsc is the CALLING party.
     * Transmits the low band (1200 Hz), listens to the high band (2400 Hz).
     */
    g.modem = v22bis_init(NULL,
                          2400,
                          0,              /* guard tone: none          */
                          1,              /* calling_party: YES        */
                          modem_get_bit,  /* TX: spandsp pulls from us */
                          NULL,
                          modem_put_bit,  /* RX: spandsp pushes to us  */
                          NULL);

    if (!g.modem) {
        LOG("ERROR: v22bis_init() failed");
        return -1;
    }

    g.carrier_count = 0;
    return 0;
}

static void modem_destroy(void)
{
    if (g.modem) {
        v22bis_free(g.modem);
        g.modem = NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  SIP mode — TX audio, on a fixed 20ms heartbeat                      */
/* ------------------------------------------------------------------ */
static void send_tx_frame(void)
{
    int16_t pcm[FRAME_SAMPLES];

    if (!g.call || !g.modem) return;
    memset(pcm, 0, sizeof(pcm));
    v22bis_tx(g.modem, pcm, FRAME_SAMPLES);
    sip_send_audio(g.call, pcm, FRAME_SAMPLES);
    g.frames_sent++;
}

/* ------------------------------------------------------------------ */
/*  SIP callbacks                                                       */
/* ------------------------------------------------------------------ */
static void on_sip_log(const char *msg)
{
    LOG("%s", msg);
}

static void on_sip_answered(sip_call_t *c)
{
    if (c != g.call) return;
    LOG("Call answered — starting V.22bis (timeout=%ds)", HANDSHAKE_TIMEOUT_SEC);
    LOG_CONN(LOG_NOTICE, "Call answered: V.22bis handshake timeout=%ds",
             HANDSHAKE_TIMEOUT_SEC);
    g.cstate     = CSTATE_HANDSHAKE;
    g.call_start = time(NULL);
    clock_gettime(CLOCK_MONOTONIC, &g.next_tx);
}

static void on_sip_audio(sip_call_t *c, const int16_t *pcm, int n)
{
    if (c != g.call || !TX_ACTIVE(g.cstate)) return;
    g.frames_recv++;
    v22bis_rx(g.modem, (int16_t *)pcm, n);

    if (g.cstate == CSTATE_CONNECTED && !g.sync_done &&
        g.sample_count >= g_cfg.min_samples)
        g.cstate = (try_set_clock() == 0) ? CSTATE_DONE : CSTATE_HANGUP;
}

/* Lost RTP packets: keep the demodulator's timing */
static void on_sip_lost(sip_call_t *c, int samples)
{
    if (c != g.call || !TX_ACTIVE(g.cstate)) return;
    v22bis_rx_fillin(g.modem, samples);
}

static void call_ended(const char *why)
{
    long dur = g.call_start ? (long)(time(NULL) - g.call_start) : 0;
    LOG("Call ended (%s): duration=%lds sent=%lu recv=%lu",
        why, dur, g.frames_sent, g.frames_recv);
    LOG_CONN(LOG_NOTICE, "Call ended (%s): duration=%lds sent=%lu recv=%lu",
             why, dur, g.frames_sent, g.frames_recv);
    g.cstate = CSTATE_IDLE;
    g.call   = NULL;
}

static void on_sip_ended(sip_call_t *c, const char *why)
{
    if (c == g.call) call_ended(why);
}

/* ------------------------------------------------------------------ */
/*  SIP sync attempt                                                    */
/* ------------------------------------------------------------------ */
static int sip_sync_attempt(void)
{
    LOG("SIP sync: calling %s", g_cfg.number);
    LOG_CONN(LOG_INFO, "Calling: %s", g_cfg.number);

    /* Reset per-call state */
    g.cstate        = CSTATE_CALLING;
    g.call_start    = 0;
    g.frames_sent   = 0;
    g.frames_recv   = 0;
    g.sync_done     = 0;
    g.line_len      = 0;
    g.line_buf[0]   = '\0';
    g.sample_count  = 0;
    g.rx_char_state = RX_STATE_IDLE;

    if (modem_init() < 0) return -1;

    g.call = sip_dial(g_cfg.number);
    if (!g.call) {
        modem_destroy();
        g.cstate = CSTATE_IDLE;
        return -1;
    }

    time_t call_placed = time(NULL);

    /* ---- main call event loop ---- */
    while (g.running && !g_got_sigterm && g.cstate != CSTATE_IDLE) {

        if (g_got_sighup) {
            g_got_sighup = 0;
            logs_reopen();
        }

        long since_answer = g.call_start ? (long)(time(NULL) - g.call_start) : 0;

        if (g.cstate == CSTATE_CALLING &&
            (long)(time(NULL) - call_placed) >= g_cfg.dial_timeout) {
            LOG("ERROR: no answer within %ds", g_cfg.dial_timeout);
            g.cstate = CSTATE_HANGUP;
        } else if (g.cstate == CSTATE_HANDSHAKE &&
                   since_answer >= HANDSHAKE_TIMEOUT_SEC) {
            LOG("ERROR: V.22bis handshake timeout after %ds "
                "(carrier events: %d)",
                HANDSHAKE_TIMEOUT_SEC, g.carrier_count);
            LOG_CONN(LOG_ERR, "V.22bis handshake timeout after %ds",
                     HANDSHAKE_TIMEOUT_SEC);
            g.cstate = CSTATE_HANGUP;
        } else if (g.cstate == CSTATE_CONNECTED &&
                   since_answer >= g_cfg.call_timeout) {
            LOG("Call timeout after %lds with %d samples",
                since_answer, g.sample_count);
            LOG_CONN(LOG_WARNING,
                     "Call timeout after %lds without sufficient samples",
                     since_answer);
            g.cstate = CSTATE_HANGUP;
        }

        if (g.cstate == CSTATE_DONE || g.cstate == CSTATE_HANGUP) {
            sip_hangup(g.call);
            call_ended("local hangup");
            break;
        }

        if (TX_ACTIVE(g.cstate) && ms_until(&g.next_tx) <= 0) {
            send_tx_frame();
            ts_add_ms(&g.next_tx, FRAME_MS);
            if (ms_until(&g.next_tx) < -FRAME_MS)
                clock_gettime(CLOCK_MONOTONIC, &g.next_tx);
        }

        long wait_ms = TX_ACTIVE(g.cstate) ? ms_until(&g.next_tx) : FRAME_MS;
        if (wait_ms > FRAME_MS) wait_ms = FRAME_MS;
        if (wait_ms < 0)        wait_ms = 0;
        sip_poll((int)wait_ms);
    }

    /* Cleanup (interrupted by a signal) */
    if (g.call) {
        sip_hangup(g.call);
        call_ended("shutdown");
    }
    modem_destroy();
    g.cstate = CSTATE_IDLE;

    return g.sync_done ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/*  TTY mode — serial port helpers                                      */
/* ------------------------------------------------------------------ */
static int tty_open(const char *device, int baud_flag)
{
    int fd = open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        LOG("ERROR: Cannot open %s: %s", device, strerror(errno));
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        LOG("ERROR: fcntl() on %s: %s", device, strerror(errno));
        close(fd);
        return -1;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) < 0) {
        LOG("ERROR: tcgetattr(%s): %s", device, strerror(errno));
        close(fd);
        return -1;
    }

    cfmakeraw(&tio);
    cfsetispeed(&tio, baud_flag);
    cfsetospeed(&tio, baud_flag);

    tio.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tio.c_cflag |=  CS8 | CLOCAL | CREAD | HUPCL;
    tio.c_cc[VMIN]  = 1;
    tio.c_cc[VTIME] = 0;

    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        LOG("ERROR: tcsetattr(%s): %s", device, strerror(errno));
        close(fd);
        return -1;
    }

    tcflush(fd, TCIOFLUSH);
    LOG("TTY opened: %s", device);
    return fd;
}

static int tty_at_command(int fd, const char *cmd, int timeout_sec)
{
    char line[256];
    int  line_len = 0;
    char tx[128];

    snprintf(tx, sizeof(tx), "%s\r", cmd);
    LOG("AT>>> %s", cmd);
    if (write(fd, tx, strlen(tx)) < 0) {
        LOG("ERROR: write(%s): %s", cmd, strerror(errno));
        return -1;
    }

    time_t deadline = time(NULL) + timeout_sec;
    fd_set rfd;

    while (time(NULL) < deadline) {
        struct timeval tv = { 1, 0 };
        FD_ZERO(&rfd);
        FD_SET(fd, &rfd);

        int r = select(fd + 1, &rfd, NULL, NULL, &tv);
        if (r <= 0) continue;

        uint8_t c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) continue;

        if (c == '\r' || c == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                LOG("AT<<< %s", line);
                if (strncmp(line, "OK",          2) == 0) return  0;
                if (strncmp(line, "ERROR",        5) == 0) return -1;
                if (strncmp(line, "NO DIALTONE", 11) == 0) return -1;
                if (strncmp(line, "NO CARRIER",  10) == 0) return -1;
                line_len = 0;
            }
        } else if (c >= 0x20 && line_len < (int)sizeof(line) - 1) {
            line[line_len++] = (char)c;
        }
    }

    LOG("WARN: AT command '%s' timed out after %ds", cmd, timeout_sec);
    return -1;
}

static int tty_wait_connect(int fd, int timeout_sec)
{
    char   line[256];
    int    line_len = 0;
    time_t deadline = time(NULL) + timeout_sec;
    fd_set rfd;

    while (time(NULL) < deadline) {
        struct timeval tv = { 1, 0 };
        FD_ZERO(&rfd);
        FD_SET(fd, &rfd);

        int r = select(fd + 1, &rfd, NULL, NULL, &tv);
        if (r <= 0) continue;

        uint8_t c;
        ssize_t n = read(fd, &c, 1);
        if (n <= 0) continue;

        if (c == '\r' || c == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                LOG("MODEM: %s", line);
                if (strncmp(line, "CONNECT",     7) == 0) {
                    LOG_CONN(LOG_NOTICE,
                             "Physical modem connected: %s", line);
                    return 0;
                }
                if (strncmp(line, "NO CARRIER",  10) == 0 ||
                    strncmp(line, "NO DIALTONE", 11) == 0 ||
                    strncmp(line, "ERROR",         5) == 0 ||
                    strncmp(line, "BUSY",          4) == 0) {
                    LOG("Connection failed: %s", line);
                    LOG_CONN(LOG_ERR, "Connection failed: %s", line);
                    return -1;
                }
                line_len = 0;
            }
        } else if (c >= 0x20 && line_len < (int)sizeof(line) - 1) {
            line[line_len++] = (char)c;
        }
    }

    LOG("ERROR: Dial timeout after %ds", timeout_sec);
    LOG_CONN(LOG_ERR, "Dial timeout after %ds", timeout_sec);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  TTY mode — wait for device to appear                                */
/*                                                                      */
/*  At boot the kernel may not have finished USB enumeration when the   */
/*  service starts.  Poll for up to timeout_sec seconds rather than     */
/*  failing immediately with ENOENT.                                    */
/* ------------------------------------------------------------------ */
static int tty_wait_device(const char *device, int timeout_sec)
{
    time_t deadline = time(NULL) + timeout_sec;
    int    warned   = 0;

    while (time(NULL) < deadline) {

        if (g_got_sigterm) return -1;

        if (access(device, F_OK) == 0) {
            if (warned)
                LOG("Device appeared: %s", device);
            return 0;
        }

        if (!warned) {
            LOG("Waiting for device %s (up to %ds)...",
                device, timeout_sec);
            LOG_CONN(LOG_WARNING,
                     "Device %s not present — waiting up to %ds",
                     device, timeout_sec);
            warned = 1;
        }

        sleep(1);
    }

    LOG("ERROR: Device %s did not appear within %ds — "
        "check cable / TTY config", device, timeout_sec);
    LOG_CONN(LOG_ERR,
             "Device %s absent after %ds wait", device, timeout_sec);
    return -1;
}

/*
 * Hang up the modem.  Dropping DTR returns a modem in the usual &D2
 * setting to command mode at once; the +++ / ATH escape is only used
 * if carrier detect shows the modem is still online.
 */
static void tty_hangup(int fd)
{
    int dtr = TIOCM_DTR;
    int status;

    if (ioctl(fd, TIOCMBIC, &dtr) == 0) {
        usleep(1000000);
        ioctl(fd, TIOCMBIS, &dtr);
        usleep(200000);
        if (ioctl(fd, TIOCMGET, &status) == 0 && !(status & TIOCM_CD)) {
            tcflush(fd, TCIFLUSH);
            LOG("Modem hung up (DTR)");
            return;
        }
    }

    LOG("Modem still online - using +++ escape");
    usleep(1100000);
    WRITE_DISCARD(fd, "+++", 3);
    usleep(1100000);
    tcflush(fd, TCIFLUSH);
    tty_at_command(fd, "ATH", 5);
}

/* ------------------------------------------------------------------ */
/*  TTY mode — single synchronisation attempt                           */
/* ------------------------------------------------------------------ */
static int tty_sync_attempt(void)
{
    LOG("TTY sync: device=%s number=%s",
        g_cfg.tty_device, g_cfg.number);
    LOG_CONN(LOG_INFO, "TTY sync: device=%s number=%s",
             g_cfg.tty_device, g_cfg.number);

    if (tty_wait_device(g_cfg.tty_device, g_cfg.dial_timeout) < 0)
        return -1;

    g.line_len     = 0;
    g.line_buf[0]  = '\0';
    g.sample_count = 0;
    g.sync_done    = 0;

    int fd = tty_open(g_cfg.tty_device, g_cfg.tty_baud_flag);
    if (fd < 0) return -1;
    g.tty_fd = fd;

    /* AT initialisation commands (comma-separated) */
    char init_copy[256];
    snprintf(init_copy, sizeof(init_copy), "%s", g_cfg.at_init);

    char *tok = strtok(init_copy, ",");
    while (tok) {
        while (*tok == ' ' || *tok == '\t') tok++;
        if (*tok) {
            if (tty_at_command(fd, tok, 5) < 0)
                LOG("WARN: AT init '%s' failed (continuing)", tok);
        }
        tok = strtok(NULL, ",");
    }

    /* Dial */
    char dial_cmd[192];
    snprintf(dial_cmd, sizeof(dial_cmd), "%s%s\r",
             g_cfg.dial_prefix, g_cfg.number);

    LOG("Dialling: %s%s", g_cfg.dial_prefix, g_cfg.number);
    LOG_CONN(LOG_INFO, "Dialling: %s%s", g_cfg.dial_prefix, g_cfg.number);
    LOG_MSG(LOG_NOTICE, "--- BEGIN CALL to %s ---", g_cfg.number);

    if (write(fd, dial_cmd, strlen(dial_cmd)) < 0) {
        LOG("ERROR: Dial write failed: %s", strerror(errno));
        close(fd);
        g.tty_fd = -1;
        return -1;
    }

    if (tty_wait_connect(fd, g_cfg.dial_timeout) < 0) {
        tty_hangup(fd);
        close(fd);
        g.tty_fd = -1;
        return -1;
    }

    LOG("Connected — receiving ACTS time strings");

    time_t call_start = time(NULL);
    int    result     = -1;
    fd_set rfd;

    while (g.running && !g_got_sigterm) {

        if (g_got_sighup) {
            g_got_sighup = 0;
            logs_reopen();
        }

        long elapsed = (long)(time(NULL) - call_start);
        if (elapsed >= g_cfg.call_timeout) {
            LOG("Call timeout after %lds", elapsed);
            LOG_CONN(LOG_WARNING,
                     "Call timeout after %lds without sufficient samples",
                     elapsed);
            break;
        }

        struct timeval tv = { 1, 0 };
        FD_ZERO(&rfd);
        FD_SET(fd, &rfd);

        int r = select(fd + 1, &rfd, NULL, NULL, &tv);
        if (r < 0) {
            if (errno == EINTR) continue;
            LOG("ERROR: select() on TTY: %s", strerror(errno));
            break;
        }
        if (r == 0) continue;

        uint8_t buf[256];
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            LOG("ERROR: read() on TTY: %s", strerror(errno));
            break;
        }
        if (n == 0) {
            LOG("TTY: EOF on %s", g_cfg.tty_device);
            break;
        }

        for (ssize_t i = 0; i < n; i++)
            feed_char(buf[i]);

        if (g.sample_count >= g_cfg.min_samples) {
            result = try_set_clock();
            break;
        }
    }

    /* Hang up first, so the modem is not online when the server
       drops its carrier (which makes modems print line noise) */
    tty_hangup(fd);
    close(fd);
    g.tty_fd = -1;

    long duration = (long)(time(NULL) - call_start);
    LOG_MSG(LOG_NOTICE, "--- END CALL (duration=%lds) ---", duration);
    LOG_CONN(LOG_NOTICE,
             "Call ended: duration=%lds samples=%d synced=%d",
             duration, g.sample_count, g.sync_done);

    return result;
}

/* ------------------------------------------------------------------ */
/*  Unified sync attempt dispatcher                                     */
/* ------------------------------------------------------------------ */
static int do_sync_attempt(void)
{
    g.sync_attempted++;

    LOG("=== Sync attempt #%d (mode=%s) ===",
        g.sync_attempted,
        g_cfg.mode == MODE_SIP ? "sip" : "tty");
    LOG_SYNC(LOG_INFO, "Sync attempt #%d mode=%s",
             g.sync_attempted,
             g_cfg.mode == MODE_SIP ? "sip" : "tty");

    int result = (g_cfg.mode == MODE_SIP)
               ? sip_sync_attempt()
               : tty_sync_attempt();

    if (result == 0) {
        LOG("=== Sync attempt #%d SUCCEEDED ===", g.sync_attempted);
        LOG_SYNC(LOG_NOTICE,
                 "Sync attempt #%d succeeded", g.sync_attempted);
    } else {
        LOG("=== Sync attempt #%d FAILED ===", g.sync_attempted);
        LOG_SYNC(LOG_WARNING,
                 "Sync attempt #%d failed", g.sync_attempted);
    }

    return result;
}

/* ------------------------------------------------------------------ */
/*  Main                                                                */
/* ------------------------------------------------------------------ */
int main(int argc, char *argv[])
{
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.mode = MODE_SIP;
    snprintf(g_cfg.config_file, sizeof(g_cfg.config_file),
             "%s", DEFAULT_CONFIG_FILE);

    /* First CLI pass finds --config; the second makes the command
       line override the config file */
    if (cli_parse(argc, argv, &g_cfg) < 0)
        return EXIT_FAILURE;

    if (config_parse(&g_cfg) < 0)
        return EXIT_FAILURE;

    optind = 0;
    if (cli_parse(argc, argv, &g_cfg) < 0)
        return EXIT_FAILURE;

    config_apply_defaults(&g_cfg);

    if (config_validate(&g_cfg) < 0) {
        fprintf(stderr,
                "\nMissing configuration. Provide values via:\n"
                "  Config file : %s\n"
                "  CLI options : %s --help\n",
                g_cfg.config_file, argv[0]);
        return EXIT_FAILURE;
    }

    openlog("actsc", LOG_PID | LOG_CONS, LOG_DAEMON);

    if (logs_open() < 0) {
        syslog(LOG_ERR, "Failed to open log files in %s", LOG_DIR);
        closelog();
        return EXIT_FAILURE;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    memset(&g, 0, sizeof(g));
    g.running = 1;
    g.cstate  = CSTATE_IDLE;
    g.tty_fd  = -1;

    LOG("actsc %s starting", ACTSC_VERSION);
    LOG("  Config file    : %s", g_cfg.config_file);
    LOG("  Mode           : %s",
        g_cfg.mode == MODE_SIP
        ? "SIP (software V.22bis over RTP)"
        : "TTY (physical modem)");

    if (g_cfg.mode == MODE_SIP) {
        LOG("  Registrar      : %s", g_cfg.registrar[0] ? g_cfg.registrar : "(none)");
        LOG("  Username       : %s", g_cfg.username[0] ? g_cfg.username : "(none)");
        LOG("  Number         : %s", g_cfg.number);
        LOG("  SIP / RTP ports: %d/udp, %d-%d/udp",
            g_cfg.sip_port, g_cfg.rtp_min, g_cfg.rtp_max);
    } else {
        LOG("  Device         : %s",    g_cfg.tty_device);
        LOG("  AT init        : %s",
            g_cfg.at_init[0] ? g_cfg.at_init : "(none)");
        LOG("  Dial prefix    : %s",    g_cfg.dial_prefix);
        LOG("  Number         : %s",    g_cfg.number);
    }

    LOG("  Service mode   : %s",
        g_cfg.service_mode ? "yes" : "no (one-shot)");
    if (g_cfg.service_mode)
        LOG("  Poll interval  : %ds", g_cfg.poll_interval);
    LOG("  Min samples    : %d",    g_cfg.min_samples);
    LOG("  Max offset     : %ds",   g_cfg.max_offset_sec);
    LOG("  Call timeout   : %ds",   g_cfg.call_timeout);
    LOG("  Dial timeout   : %ds",   g_cfg.dial_timeout);
    LOG("  Dry run        : %s",    g_cfg.dry_run ? "YES" : "no");
    LOG("  Logs           : %s",    LOG_DIR);

    LOG_SYNC(LOG_INFO,
             "actsc %s starting: mode=%s service=%d interval=%d "
             "min_samples=%d dry_run=%d",
             ACTSC_VERSION,
             g_cfg.mode == MODE_SIP ? "sip" : "tty",
             g_cfg.service_mode,
             g_cfg.poll_interval,
             g_cfg.min_samples,
             g_cfg.dry_run);

    if (g_cfg.mode == MODE_SIP) {
        /* Outgoing calls only: no registration, incoming calls refused */
        sip_config_t sc = {
            .port = g_cfg.sip_port, .registrar = g_cfg.registrar, .do_register = 0,
            .username = g_cfg.username, .password = g_cfg.password,
            .local_ip = g_cfg.local_ip, .public_ip = g_cfg.public_ip, .allow = "",
            .rtp_port_min = g_cfg.rtp_min, .rtp_port_max = g_cfg.rtp_max,
            .user_agent = "actsc/" ACTSC_VERSION,
        };
        sip_handlers_t sh = {
            .on_answered = on_sip_answered, .on_audio = on_sip_audio,
            .on_lost = on_sip_lost, .on_ended = on_sip_ended, .log = on_sip_log,
        };
        if (sip_init(&sc, &sh) < 0) {
            logs_close();
            closelog();
            return EXIT_FAILURE;
        }
    }

    int exit_code = EXIT_SUCCESS;

    if (!g_cfg.service_mode) {
        /* ---- One-shot ---- */
        if (do_sync_attempt() < 0) {
            LOG("One-shot sync FAILED");
            exit_code = EXIT_FAILURE;
        } else {
            LOG("One-shot sync SUCCEEDED");
        }

    } else {
        /* ---- Service loop ---- */
        LOG("Entering service loop (poll every %ds)",
            g_cfg.poll_interval);

        while (g.running && !g_got_sigterm) {

            if (g_got_sighup) {
                g_got_sighup = 0;
                logs_reopen();
            }

            do_sync_attempt();

            time_t sleep_until = time(NULL) + g_cfg.poll_interval;
            LOG("Next sync in %ds", g_cfg.poll_interval);
            LOG_SYNC(LOG_INFO, "Next sync in %ds", g_cfg.poll_interval);

            while (g.running && !g_got_sigterm) {
                if (g_got_sighup) {
                    g_got_sighup = 0;
                    logs_reopen();
                }
                if (time(NULL) >= sleep_until) break;
                sleep(1);
            }
        }

        LOG("Service loop exited");
    }

    if (g_cfg.mode == MODE_SIP)
        sip_shutdown();

    LOG("actsc stopped: attempts=%d", g.sync_attempted);
    LOG_SYNC(LOG_INFO, "actsc stopped: attempts=%d", g.sync_attempted);

    logs_close();
    closelog();
    return exit_code;
}

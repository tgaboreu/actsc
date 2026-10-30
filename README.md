# actsc — Automated Computer Time Service (ACTS) Client

A NIST-compatible ACTS time client that synchronises the system clock
via a V.22bis 2400bps modem connection to an ACTS-compatible time server
such as [actsd](https://time.tgabor.eu).

Two modem modes are supported:

- **SIP mode** — places a SIP call (directly, or through any SIP PBX
  or provider) and runs a software V.22bis modem over G.711 RTP with
  spandsp.  No physical modem hardware required.

- **TTY mode** — drives a physical Hayes-compatible modem attached to a
  serial port (e.g. `/dev/ttyUSB0`).  Sends user-supplied AT
  initialisation commands, dials, then reads plain ASCII ACTS data
  from the modem's transparent data path.

**Service:** [time.tgabor.eu](https://time.tgabor.eu)  
**Contact:** `info@tgabor.eu`  
**Version:** 2.0.0

---

## How It Works

### SIP mode

```
actsc ── SIP + RTP ── [SIP PBX / provider] ── VoIP/PSTN ── actsd (or NIST ACTS)
  │                                            │
spandsp V.22bis                        spandsp V.22bis
  │                                            │
  └── RX: ACTS time strings ──────────────────┘
  └── parse OTMs ── validate ── clock_settime()
```

### TTY mode

```
actsc ── /dev/ttyUSB0 ── physical modem ── PSTN ── actsd (or NIST ACTS)
  │                                                      │
AT commands + dial                               V.22bis 2400bps
  │                                                      │
  └── RX: ACTS time strings ────────────────────────────┘
  └── parse OTMs ── validate ── clock_settime()
```

---

## ACTS Time Message Format

```
                                 D  L
  MJD    YR MO DA H  M  S     ST S DUT1  msADV          <OTM>
 61277 26-08-25 07:56:44       50 0 +.0 050.0 UTC(ACTS) *
```

| Field | Description |
|-------|-------------|
| MJD | Modified Julian Day number |
| YR-MO-DA | UTC date |
| H:M:S | UTC time (one second ahead — valid on receipt of `*`) |
| DST | `00` = standard time, `50` = DST in effect |
| LS | Leap second flag: `0` = none, `1` = add, `2` = drop |
| DUT1 | UT1 − UTC in 0.1s steps |
| msADV | On-time marker advance (ms) |
| `*` | On-time marker — time is valid at the moment of receipt |

actsc captures the system wall clock at the instant `*` is received,
computes the offset, collects `min_samples` such measurements, validates
that they are consecutive and consistent, then applies the median offset
via `clock_settime()`.

---

## Dependencies

Both are standard distribution packages:

| Library | Debian / Ubuntu | Fedora / RHEL | Purpose |
|---------|-----------------|---------------|---------|
| spandsp | `libspandsp-dev` | `spandsp-devel` | V.22bis software modem, G.711 |
| Sofia-SIP | `libsofia-sip-ua-dev` | `sofia-sip-devel` | SIP signalling |

```bash
apt install build-essential pkg-config libspandsp-dev libsofia-sip-ua-dev   # Debian / Ubuntu
dnf install gcc make pkgconf spandsp-devel sofia-sip-devel                     # Fedora
pacman -S base-devel spandsp sofia-sip                                         # Arch
```

One binary supports both modes, so both libraries are needed even if
you only use TTY mode.

In SIP mode, actsc calls either through a SIP PBX or provider that
routes the number to an ACTS server (actsd, or a PSTN gateway to a
real ACTS service), or directly to a `sip:` URI.

---

## Building

```bash
make
```

The Makefile finds the libraries with pkg-config and checks the
headers first.  For a library installed outside the system paths:

```bash
make PKG_CONFIG_PATH=/opt/sofia-sip/lib/pkgconfig
```

`sip.c` / `sip.h` hold the SIP user agent and RTP code.  They are
shared with actsd and utbd, and identical in all three projects.

---

## Installation

### Install binary and configuration

```bash
make install
```

This will:

- Install the binary to `/usr/local/sbin/actsc`
- Create the dedicated `actsc` system user and group
- Create `/etc/actsc/` and install `actsc.conf` from the sample
- Create `/var/log/actsc/`
- Install logrotate configuration to `/etc/logrotate.d/actsc`

If `/etc/actsc/actsc.conf` already exists it will not be overwritten.
The sample will be placed alongside it as `actsc.conf.sample` for
reference.

### Install binary, configuration, and systemd service

```bash
make install-service
```

This runs `make install` and additionally:

- Installs `/etc/systemd/system/actsc.service`
- Runs `systemctl daemon-reload`

### TTY mode: serial port access

The `actsc` system user must be a member of the `dialout` group to
open serial devices:

```bash
usermod -aG dialout actsc
```

### Clock setting privileges

`clock_settime()` requires `CAP_SYS_TIME`. The systemd unit grants
this capability via `AmbientCapabilities` — no setuid binary is needed.

For one-shot use outside of systemd, either run as root or grant the
file capability to the binary:

```bash
setcap cap_sys_time=ep /usr/local/sbin/actsc
```

Verify:

```bash
getcap /usr/local/sbin/actsc
# actsc = cap_sys_time+ep
```

---

## Configuration

actsc has no hardcoded defaults for connection parameters.
All required values must be provided either via a configuration
file or command-line arguments.

### Configuration file

The default configuration file is `/etc/actsc/actsc.conf`.

```ini
# /etc/actsc/actsc.conf

# Modem mode: sip or tty
mode = sip

# --- SIP mode ---
registrar = pbx.example.com     # PBX / provider that routes the call
username  = actsc               # SIP account for digest authentication
password  = changeme
number    = 1234                # extension, or sip:user@host for a direct call
# sip_port  = 5062
# rtp_ports = 10100-10199
# local_ip  = 192.0.2.7         # default: the address routing to the registrar
# public_ip = 203.0.113.7       # only behind NAT

# --- TTY mode ---
# tty         = /dev/ttyUSB0
# at_init     = AT,ATZ,ATX3,ATE0,ATM0
# dial_prefix = ATDT
# number      = 5551234

# --- Common ---
# service_mode   = no
# poll_interval  = 3600
# min_samples    = 10
# max_offset_sec = 300
# call_timeout   = 60
# dial_timeout   = 45
# dry_run        = no
```

- Lines beginning with `#` are comments
- Leading and trailing whitespace around keys and values is ignored
- Unknown keys produce a warning but do not cause actsc to exit

A fully annotated sample is installed to `/etc/actsc/actsc.conf`
during `make install`.

### Command-line options

All configuration file values can be overridden on the command line.
CLI arguments always take priority.

```
Usage: actsc [OPTIONS]

Options:
  -m, --mode=MODE          Modem mode: sip or tty (default: sip)

SIP mode options:
  -r, --registrar=HOST     SIP PBX / provider that routes the call
  -u, --username=USER      SIP user name
  -w, --password=PASS      SIP password
  -p, --sip-port=PORT      Local SIP port (default: 5062)
  -N, --number=NUMBER      Extension, or sip:user@host for a direct call

TTY mode options:
  -t, --tty=DEVICE         Serial device (e.g. /dev/ttyUSB0)
  -A, --at-init=CMDS       Comma-separated AT init commands
                           e.g. "AT,ATX3,ATE0,ATM0"
  -P, --dial-prefix=PFX    Dial command prefix (default: ATDT)
  -B, --baud=RATE          Serial port baud rate (default: 2400)
  -N, --number=NUMBER      Phone number to dial

Common options:
  -c, --config=FILE        Configuration file
                           (default: /etc/actsc/actsc.conf)
  -S, --service            Run as a daemon (poll loop)
  -i, --interval=SECS      Poll interval in service mode
                           (default: 3600)
      --min-samples=N      Valid OTMs required before setting clock
                           (default: 10, max: 16)
      --max-offset=SECS    Reject offsets larger than this
                           (default: 300)
      --call-timeout=SECS  Abort call after this many seconds
                           (default: 60)
      --dry-run            Parse and log but do NOT set clock
  -h, --help               Display this help and exit
  -V, --version            Display version information and exit
```

### Configuration precedence

```
CLI arguments  >  configuration file  >  built-in defaults (optional fields only)
```

If a required value is not set by any of these, actsc will print an
error and refuse to start.

---

## Running

### As a systemd service (recommended)

```bash
# Edit configuration first
nano /etc/actsc/actsc.conf

# Enable and start
systemctl enable --now actsc

# Check status
systemctl status actsc

# Follow live output
journalctl -fu actsc
```

### One-shot with configuration file

```bash
# Use default config file
actsc

# Use a custom config file
actsc --config /etc/actsc/actsc-test.conf

# Override individual values
actsc --config /etc/actsc/actsc.conf --dry-run
```

### One-shot SIP mode without configuration file

```bash
# Through a PBX / provider
actsc --mode sip \
      --registrar pbx.example.com \
      --username actsc \
      --password mysecret \
      --number 1234

# Directly to an actsd server
actsc --mode sip --number sip:time@192.0.2.5
```

### One-shot TTY mode without configuration file

```bash
actsc --mode tty \
      --tty /dev/ttyUSB0 \
      --at-init "AT,ATZ,ATX3,ATE0,ATM0" \
      --dial-prefix ATDT \
      --number 5551234
```

### Dry run (test without setting the clock)

```bash
actsc --dry-run \
      --mode sip \
      --registrar pbx.example.com \
      --username actsc \
      --password mysecret \
      --number 1234
```

---

## AT Command Initialisation (TTY mode)

The `at_init` option (or `--at-init` on the CLI) accepts a
comma-separated list of AT commands. Each command is sent to the
modem individually. actsc waits for `OK` or `ERROR` after each one
before proceeding.

### Common AT commands

| Command | Purpose |
|---------|---------|
| `AT` | Attention — basic connectivity test |
| `ATZ` | Full modem reset to factory defaults |
| `ATE0` | Disable command echo |
| `ATM0` | Disable speaker |
| `ATX3` | Disable dial-tone detection (recommended for VoIP lines) |
| `ATB0` | Select CCITT modulation (V.22bis) — modem-dependent |
| `ATV1` | Verbose result codes (`OK`, `ERROR`, `CONNECT`) |
| `ATQ0` | Enable result codes |

### Example configurations

```ini
# Minimal — just reset and suppress echo
at_init = ATZ,ATE0

# VoIP line — disable dial-tone detection and speaker
at_init = ATZ,ATE0,ATM0,ATX3

# Force V.22bis on modems that default to V.90/V.92
at_init = ATZ,ATE0,ATM0,ATX3,ATB0
```

### Dial prefix

```ini
# Tone dialling (default)
dial_prefix = ATDT

# Pulse dialling
dial_prefix = ATDP
```

---

## Clock Synchronisation Logic

### Sampling

actsc collects one time sample per received on-time marker (`*`).
At each OTM the system wall clock (`CLOCK_REALTIME`) is read as close
to the OTM as possible and compared to the ACTS-reported UTC time.

```
offset_ms = wall_clock_ms − acts_time_ms
```

A positive offset means the system clock is fast; negative means slow.

### Validation

Before setting the clock, actsc performs two checks:

1. **Consecutive OTMs** — the ACTS timestamps of successive samples
   must differ by exactly one second. A gap indicates line noise,
   a misparse, or a partial string received at connection start.
   A gap starts a new series with the sample that broke it.

2. **Sanity limit** — the median offset must not exceed
   `max_offset_sec` (default 300 seconds). An offset larger than
   this almost certainly indicates a configuration error, a severely
   wrong system clock, or a parse failure. actsc logs an error and
   does not set the clock.

### Correction

Once `min_samples` consecutive, consistent OTMs have been collected:

1. Offsets are sorted and the **median** is taken (robust against
   outliers caused by momentary line noise or scheduler jitter).

2. The system clock is stepped by `−median_offset` with
   `clock_settime(CLOCK_REALTIME)`, keeping millisecond resolution.

The ACTS line carries whole seconds, but the on-time marker itself
defines the instant within the second, so the result is accurate to
the jitter of the path: typically a few tens of milliseconds over
SIP/VoIP, plus the serial port latency in TTY mode (USB serial adapters
can buffer up to 16ms; `setserial /dev/ttyUSB0 low_latency` helps).

### Recommended workflow

```
actsc (coarse sync, ±500ms)  →  ntpd / chrony (fine sync, <10ms)
```

Configure NTP to start after actsc has run at least once, or use
`chrony`'s `makestep` directive to accept a large initial correction.

---

## Logging

actsc writes to three log destinations simultaneously:

| Destination | Contents | Command |
|-------------|----------|---------|
| stdout | All events (standalone use) | — |
| systemd journal | All events with `[CATEGORY]` prefix | `journalctl -fu actsc` |
| `/var/log/actsc/sync` | Clock sync events and results | `tail -f /var/log/actsc/sync` |
| `/var/log/actsc/connections` | Call setup and teardown | `tail -f /var/log/actsc/connections` |
| `/var/log/actsc/messages` | Received ACTS strings | `tail -f /var/log/actsc/messages` |

### Log format

All log file entries are timestamped in UTC:

```
[YYYY-MM-DD HH:MM:SS UTC] message
```

### Example output

```
/var/log/actsc/sync
────────────────────────────────────────────────────────────
[2026-10-25 07:56:00 UTC] actsc 2.0.0 starting: mode=sip service=0 interval=3600 min_samples=10 dry_run=0
[2026-10-25 07:56:00 UTC] Sync attempt #1 mode=sip
[2025-08-25 07:56:52 UTC] Sample #1: acts_utc=1756087012 offset=+1240ms
[2025-08-25 07:56:53 UTC] Sample #2: acts_utc=1756087013 offset=+1238ms
[2025-08-25 07:56:54 UTC] Sample #3: acts_utc=1756087014 offset=+1241ms
[2025-08-25 07:56:54 UTC] Clock set: utc=1756087014 correction=-1239ms samples=3
[2025-08-25 07:56:54 UTC] Sync attempt #1 succeeded

/var/log/actsc/connections
────────────────────────────────────────────────────────────
[2026-10-25 07:56:00 UTC] Calling: 1234
[2026-10-25 07:56:01 UTC] Call answered: V.22bis handshake timeout=45s
[2025-08-25 07:56:03 UTC] Carrier up: event #1
[2025-08-25 07:56:04 UTC] V.22bis training succeeded: elapsed=2s
[2025-08-25 07:56:54 UTC] Call ended: duration=54s sent=2700 recv=2700

/var/log/actsc/messages
────────────────────────────────────────────────────────────
[2025-08-25 07:56:00 UTC] --- BEGIN CALL to 1234 ---
[2025-08-25 07:56:50 UTC] RX:  61277 25-08-25 07:56:50       50 0 +.0 050.0 UTC(ACTS) *
[2025-08-25 07:56:51 UTC] RX:  61277 25-08-25 07:56:51       50 0 +.0 050.0 UTC(ACTS) *
[2025-08-25 07:56:52 UTC] RX:  61277 25-08-25 07:56:52       50 0 +.0 050.0 UTC(ACTS) *
[2025-08-25 07:56:53 UTC] RX:  61277 25-08-25 07:56:53       50 0 +.0 050.0 UTC(ACTS) *
[2025-08-25 07:56:54 UTC] --- END CALL (duration=54s) ---
```

### Log rotation

Logs are rotated weekly, compressed, and kept for 12 weeks.
Configuration is installed to `/etc/logrotate.d/actsc`.

```bash
# Test rotation manually
logrotate -f /etc/logrotate.d/actsc
```

A `SIGHUP` signal causes actsc to reopen all log file handles,
which is required for log rotation to work correctly when running
as a service. The logrotate `postrotate` script sends this signal
automatically.

---

## Troubleshooting

### Clock not being set

**Check capabilities:**

```bash
# Standalone binary
getcap /usr/local/sbin/actsc

# Service
systemctl show actsc | grep -i capability
```

**Run with dry-run first to confirm parsing works:**

```bash
actsc --dry-run --config /etc/actsc/actsc.conf
```

**Check the sync log:**

```bash
tail -f /var/log/actsc/sync
```

### SIP mode: call not connecting

The log names the SIP failure, for example `Call ended (SIP 403
Forbidden)` (wrong credentials or caller not allowed), `SIP 404 Not
Found` (number not routed) or `SIP 488 Not Acceptable Here` (no
G.711 codec in common).

```bash
# Watch the SIP traffic while actsc dials
tcpdump -n -i any -A udp port 5060 or udp port 5062

# Asterisk: show the SIP dialogue
asterisk -rx "pjsip set logger on"
```

If the call connects but the modem never trains, check that the path
uses G.711 end to end and that UDP 10100–10199 (`rtp_ports`) is
open for the media.

### TTY mode: modem not responding

```bash
# Check device exists and permissions
ls -la /dev/ttyUSB0
groups actsc

# Test modem manually
screen /dev/ttyUSB0 2400
# type AT — should see OK

# Add actsc to dialout group if missing
usermod -aG dialout actsc
```

### TTY mode: no CONNECT after dialling

| Symptom | Cause | Fix |
|---------|-------|-----|
| `NO DIALTONE` | VoIP line, no dial tone | Add `ATX3` to `at_init` |
| `BUSY` | Number engaged | Retry later |
| `NO CARRIER` | Wrong number or server down | Verify number |
| Timeout | Modem too slow to train | Increase `dial_timeout` |

### Not enough samples collected

Increase `call_timeout` to allow more time for OTM collection,
or reduce `min_samples` (minimum 1, not recommended below 3):

```ini
call_timeout = 90
min_samples  = 3
```

### Offset sanity check failing

If the system clock is severely wrong (more than `max_offset_sec`
from UTC), actsc will refuse to set it. Temporarily increase the
limit or set the clock manually first:

```bash
# Set clock manually (rough)
date -s "2025-08-25 07:56:00"

# Or increase the limit temporarily
actsc --max-offset 86400 --config /etc/actsc/actsc.conf
```

---

## Upgrading from 1.x

actsc 1.x placed calls over IAX2 through libiax2; 2.x uses SIP.
`mode = iax` becomes `mode = sip`, and:

| 1.x (IAX2) | 2.x (SIP) |
|------------|-----------|
| `host` + `port` | `registrar = host[:port]` (SIP port, usually 5060) |
| `peername` | `username` |
| `secret` | `password` |
| `number` | `number` (unchanged), or a full `sip:` URI |

On Asterisk, create a PJSIP endpoint with authentication for actsc
(allow `alaw,ulaw`) instead of the `iax.conf` peer.  TTY mode is
unchanged.

---

## Uninstalling

```bash
make uninstall
```

This stops and disables the service, removes the binary, systemd unit,
and logrotate configuration.

Configuration (`/etc/actsc/`) and logs (`/var/log/actsc/`) are
intentionally preserved and must be removed manually if no longer
needed:

```bash
rm -rf /etc/actsc /var/log/actsc
```

To remove the service user and group:

```bash
userdel  actsc
groupdel actsc
```

---

## File Layout

```
/usr/local/sbin/actsc               Binary
/etc/actsc/actsc.conf               Configuration file
/etc/actsc/actsc.conf.sample        Annotated sample (after upgrade)
/etc/systemd/system/actsc.service   Systemd unit
/etc/logrotate.d/actsc              Log rotation configuration
/var/log/actsc/sync                 Clock sync event log
/var/log/actsc/connections          Connection and handshake log
/var/log/actsc/messages             Received ACTS message log
```

---

## License

Copyright (C) 2026 Gabor Temesi <info@tgabor.eu>

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or (at
your option) any later version.

This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

The full license text is in [COPYING](COPYING).
`SPDX-License-Identifier: GPL-3.0-or-later`

### Third-party components

The program is linked with these libraries; no third-party source
code is included in this repository.

| Component | License | Project |
|-----------|---------|---------|
| spandsp | LGPL-2.1-only (library) | https://github.com/freeswitch/spandsp |
| Sofia-SIP | LGPL-2.1-or-later | https://github.com/freeswitch/sofia-sip |

Both libraries may be used by GPL-3.0 programs.  Distribution packages
of Sofia-SIP may link OpenSSL 3 (Apache-2.0), which is compatible with
GPL-3.0 as well.

**Note for binary distributors.**  Besides its LGPL-2.1 code, the
spandsp library contains a few files licensed GPL-2.0-only
(`image_translate.c` in 0.0.6; also `sprt.c` and `v150_1*.c` in newer
snapshots).  GPL-2.0-only and GPL-3.0 code cannot be combined in one
work.  This program does not use those parts, and none of this affects
building it from source or running it.  If you distribute *binaries*,
link spandsp statically (the linker then includes only the LGPL-2.1
modem objects that are used, and the LGPL-2.1 relinking terms apply)
or use a spandsp build without the GPL-2.0-only files.

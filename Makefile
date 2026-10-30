# ---------------------------------------------------------------
# actsc Makefile
# V.22bis Software Modem / NIST ACTS Time Service Client
#
# Copyright (C) 2026 Gabor Temesi <info@tgabor.eu>
# SPDX-License-Identifier: GPL-3.0-or-later
# ---------------------------------------------------------------

#
# Build dependencies:
#   spandsp  : apt install libspandsp-dev
#   Sofia-SIP: apt install libsofia-sip-ua-dev / dnf install sofia-sip-devel
#
# Usage:
#   make
#   make install          Install binary + sample config
#   make install-service  Install binary + config + systemd unit
#   make fix-permissions  Fix log file ownership (run after root testing)
#   make clean
#   make uninstall
#

# ---------------------------------------------------------------
# SIP stack — Sofia-SIP (pkg-config sofia-sip-ua)
# ---------------------------------------------------------------
PKG_CONFIG   ?= pkg-config
SOFIA_CFLAGS != $(PKG_CONFIG) --cflags sofia-sip-ua 2>/dev/null || true
SOFIA_LIBS   != $(PKG_CONFIG) --libs   sofia-sip-ua 2>/dev/null || echo "-lsofia-sip-ua"

# Without pkg-config: the headers are in a versioned directory,
# e.g. /usr/include/sofia-sip-1.12 (Debian) or sofia-sip-1.13
SOFIA_SEARCH ?= /usr/include /usr/local/include
ifeq ($(strip $(SOFIA_CFLAGS)),)
SOFIA_CFLAGS := $(addprefix -I,$(lastword $(sort $(wildcard $(addsuffix /sofia-sip-1.*,$(SOFIA_SEARCH))))))
endif

# ---------------------------------------------------------------
# spandsp — prefer pkg-config, fall back to -lspandsp
# ---------------------------------------------------------------
SPANDSP_CFLAGS != $(PKG_CONFIG) --cflags spandsp 2>/dev/null || true
SPANDSP_LIBS   != $(PKG_CONFIG) --libs   spandsp 2>/dev/null || echo "-lspandsp"

# ---------------------------------------------------------------
# Install paths
# ---------------------------------------------------------------
PREFIX      = /usr/local
BINDIR      = $(PREFIX)/sbin
UNITDIR     = /etc/systemd/system
CONFDIR     = /etc/actsc
LOGDIR      = /var/log/actsc
LOGROTDIR   = /etc/logrotate.d

# ---------------------------------------------------------------
# Unprivileged service identity
# Created automatically by 'make install'
# ---------------------------------------------------------------
ACTSC_USER  = actsc
ACTSC_GROUP = actsc

# ---------------------------------------------------------------
# Build target
# ---------------------------------------------------------------
TARGET = actsc

# ---------------------------------------------------------------
# Compiler
# ---------------------------------------------------------------
CC = gcc

# ---------------------------------------------------------------
# Security hardening flags
#
#   -fstack-protector-strong
#       Emit stack canaries for functions that have local buffers
#       or take the address of a local variable.  Detects stack
#       smashing at runtime before a corrupted return address is
#       used.
#
#   -D_FORTIFY_SOURCE=2
#       Enables glibc compile-time and runtime bounds checking on
#       memcpy, strcpy, sprintf, etc.  Requires -O1 or higher.
#       Redundant calls become no-ops; unsafe calls abort().
#
#   -fPIE
#       Compile as position-independent code so the final binary
#       can be linked as a PIE.  Required for ASLR on Linux.
#
#   -Wformat=2
#       Stricter format string checking — warns on non-literal
#       format strings and on format/argument type mismatches.
#
#   -Wformat-security
#       Subset of -Wformat=2: warns specifically on format strings
#       that could be a security risk (e.g. printf(user_string)).
# ---------------------------------------------------------------
HARDENING_CFLAGS = \
    -fstack-protector-strong \
    -D_FORTIFY_SOURCE=2 \
    -fPIE \
    -Wformat=2 \
    -Wformat-security

# ---------------------------------------------------------------
# Security hardening linker flags
#
#   -pie
#       Link as a Position-Independent Executable.  Combined with
#       -fPIE, allows the kernel to load the binary at a random
#       base address (ASLR).
#
#   -Wl,-z,relro
#       Mark ELF sections that should be read-only after dynamic
#       linking as read-only (RELRO).  Makes GOT entries and other
#       metadata harder to overwrite.
#
#   -Wl,-z,now
#       Resolve all dynamic symbols at load time rather than
#       lazily.  Combined with RELRO, the entire GOT is made
#       read-only after startup (full RELRO).
# ---------------------------------------------------------------
HARDENING_LDFLAGS = \
    -pie \
    -Wl,-z,relro \
    -Wl,-z,now

# ---------------------------------------------------------------
# Combined CFLAGS and LDFLAGS
# ---------------------------------------------------------------
CFLAGS  = -Wall -Wextra -O2 -g \
           $(SOFIA_CFLAGS) \
           $(SPANDSP_CFLAGS) \
           $(HARDENING_CFLAGS)

LDFLAGS  = $(SOFIA_LIBS) \
            $(SPANDSP_LIBS) \
            -lpthread -lm \
            $(HARDENING_LDFLAGS)

# ---------------------------------------------------------------
# Phony targets
# ---------------------------------------------------------------
.PHONY: all clean install install-service uninstall check-deps \
        create-user hardening-check fix-permissions

# ---------------------------------------------------------------
# Default target
# ---------------------------------------------------------------
all: check-deps $(TARGET)

# ---------------------------------------------------------------
# Compile
# ---------------------------------------------------------------
$(TARGET): actsc.c sip.c sip.h
	$(CC) $(CFLAGS) -o $@ actsc.c sip.c $(LDFLAGS)
	@echo ""
	@echo "Build successful: $(TARGET)"
	@echo "Install with:     make install"
	@echo "Install service:  make install-service"

# ---------------------------------------------------------------
# Dependency checks
# ---------------------------------------------------------------
check-deps:
	@echo "Checking dependencies..."
	@echo '#include <sofia-sip/nua.h>' | $(CC) $(SOFIA_CFLAGS) -E -x c - >/dev/null 2>&1 || { \
		echo "ERROR: Sofia-SIP headers not found."; \
		echo "  Debian/Ubuntu : apt install libsofia-sip-ua-dev"; \
		echo "  Fedora        : dnf install sofia-sip-devel"; \
		echo "  Arch          : pacman -S sofia-sip"; \
		exit 1; }
	@echo '#include <spandsp.h>' | $(CC) $(SPANDSP_CFLAGS) -E -x c - >/dev/null 2>&1 || { \
		echo "ERROR: spandsp headers not found (libspandsp-dev / spandsp-devel)."; \
		exit 1; }
	@echo "Dependencies OK"

# ---------------------------------------------------------------
# Hardening verification
#
# Run after build to confirm all mitigations are present in the
# compiled binary.  Requires checksec (apt install checksec).
# ---------------------------------------------------------------
hardening-check: $(TARGET)
	@echo "Checking binary hardening..."
	@if command -v checksec >/dev/null 2>&1; then \
		checksec --file=$(TARGET); \
	else \
		echo "checksec not found — checking manually with readelf..."; \
		echo ""; \
		echo "PIE:"; \
		readelf -h $(TARGET) | grep "Type:"; \
		echo ""; \
		echo "Stack canary:"; \
		readelf -s $(TARGET) 2>/dev/null | grep -q __stack_chk_fail \
		    && echo "  FOUND: __stack_chk_fail" \
		    || echo "  NOT FOUND"; \
		echo ""; \
		echo "RELRO:"; \
		readelf -l $(TARGET) | grep -i gnu_relro \
		    && echo "  FOUND" \
		    || echo "  NOT FOUND"; \
		echo ""; \
		echo "FORTIFY_SOURCE:"; \
		readelf -s $(TARGET) 2>/dev/null | grep -q _chk \
		    && echo "  FOUND: _chk symbols present" \
		    || echo "  NOT FOUND (may be optimised out if no eligible calls)"; \
		echo ""; \
		echo "Install checksec for a full report:"; \
		echo "  apt install checksec"; \
	fi

# ---------------------------------------------------------------
# Create dedicated system user and group
#
# The actsc user:
#   - Is a system account (UID < 1000, no aging)
#   - Has no login shell (/usr/sbin/nologin)
#   - Has no home directory
#   - Belongs only to its own group
#
# clock_settime() requires CAP_SYS_TIME.  This is granted via
# AmbientCapabilities in the systemd unit, NOT via a setuid
# binary.  The actsc binary itself is not setuid.
#
# The actsc user is added to the 'dialout' group automatically
# so that TTY mode can access /dev/ttyUSB0 and similar devices
# without any manual post-install step.
# ---------------------------------------------------------------
create-user:
	@echo "Creating system user/group..."
	@if ! getent group $(ACTSC_GROUP) > /dev/null 2>&1; then \
		groupadd --system $(ACTSC_GROUP); \
		echo "  Created group : $(ACTSC_GROUP)"; \
	else \
		echo "  Group exists  : $(ACTSC_GROUP)"; \
	fi
	@if ! getent passwd $(ACTSC_USER) > /dev/null 2>&1; then \
		useradd \
		    --system \
		    --gid $(ACTSC_GROUP) \
		    --no-create-home \
		    --home-dir /nonexistent \
		    --shell /usr/sbin/nologin \
		    --comment "ACTS Time Service Client" \
		    $(ACTSC_USER); \
		echo "  Created user  : $(ACTSC_USER)"; \
	else \
		echo "  User exists   : $(ACTSC_USER)"; \
	fi
	@if getent group dialout > /dev/null 2>&1; then \
		usermod -aG dialout $(ACTSC_USER); \
		echo "  Added to group: dialout (serial port access)"; \
	else \
		echo "  WARN: dialout group not found — TTY mode may lack"; \
		echo "        serial port access on this system"; \
	fi


# ---------------------------------------------------------------
# fix-permissions
#
# Creates the log directory if absent, then corrects ownership
# and mode bits on the directory itself and every file inside it.
#
# This is needed when actsc was run as root before installation
# (e.g. during initial testing), which causes log files to be
# created owned by root.  The installed service runs as
# $(ACTSC_USER) and cannot write to root-owned files.
#
# Called automatically by 'make install'.
# Can also be called standalone at any time:
#   make fix-permissions
# ---------------------------------------------------------------
fix-permissions: create-user
	@echo "Fixing log directory ownership..."
	install -d -m 750 -o $(ACTSC_USER) -g $(ACTSC_GROUP) \
	    $(DESTDIR)$(LOGDIR)
	chown -R $(ACTSC_USER):$(ACTSC_GROUP) $(DESTDIR)$(LOGDIR)
	chmod    750                           $(DESTDIR)$(LOGDIR)
	@if ls $(DESTDIR)$(LOGDIR)/* > /dev/null 2>&1; then \
		chmod 640 $(DESTDIR)$(LOGDIR)/*; \
		echo "  Fixed existing log files:"; \
		ls -la $(DESTDIR)$(LOGDIR)/; \
	else \
		echo "  Log directory is empty — nothing to fix"; \
	fi
	@echo "  Owner : $(ACTSC_USER):$(ACTSC_GROUP)"
	@echo "  Dir   : $(DESTDIR)$(LOGDIR)  mode 750"
	@echo "  Files : $(DESTDIR)$(LOGDIR)/* mode 640 (if any)"

# ---------------------------------------------------------------
# Install binary, config, logs, logrotate
# ---------------------------------------------------------------
install: $(TARGET) create-user
	@echo ""
	@echo "Installing actsc..."

	@echo "  Installing binary..."
	install -D -m 755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)

	@echo "  Creating log directory and fixing ownership..."
	$(MAKE) fix-permissions

	@echo "  Creating config directory..."
	install -d -m 755 $(DESTDIR)$(CONFDIR)

	@echo "  Installing configuration..."
	@if [ ! -f $(DESTDIR)$(CONFDIR)/actsc.conf ]; then \
		install -m 640 -o root -g $(ACTSC_GROUP) \
		    contrib/actsc.conf.sample \
		    $(DESTDIR)$(CONFDIR)/actsc.conf; \
		echo "  Created : $(DESTDIR)$(CONFDIR)/actsc.conf"; \
		echo "  *** Edit $(DESTDIR)$(CONFDIR)/actsc.conf before starting actsc ***"; \
	else \
		install -m 640 -o root -g $(ACTSC_GROUP) \
		    contrib/actsc.conf.sample \
		    $(DESTDIR)$(CONFDIR)/actsc.conf.sample; \
		echo "  Skipped : $(DESTDIR)$(CONFDIR)/actsc.conf (already exists)"; \
		echo "  Sample  : $(DESTDIR)$(CONFDIR)/actsc.conf.sample"; \
	fi

	@echo "  Installing logrotate configuration..."
	install -D -m 644 contrib/actsc.logrotate \
	    $(DESTDIR)$(LOGROTDIR)/actsc

	@echo ""
	@echo "Installation complete."
	@echo ""
	@echo "  Binary    : $(DESTDIR)$(BINDIR)/$(TARGET)"
	@echo "  Config    : $(DESTDIR)$(CONFDIR)/actsc.conf"
	@echo "  Logs      : $(DESTDIR)$(LOGDIR)/"
	@echo "  Logrotate : $(DESTDIR)$(LOGROTDIR)/actsc"
	@echo "  User      : $(ACTSC_USER):$(ACTSC_GROUP)"
	@echo ""
	@echo "TTY mode: add actsc to the dialout group for serial port access:"
	@echo "  usermod -aG dialout $(ACTSC_USER)"
	@echo ""
	@echo "Next steps:"
	@echo "  1. Edit   : $(DESTDIR)$(CONFDIR)/actsc.conf"
	@echo "  2. Install service and start:"
	@echo "             make install-service"
	@echo "  OR"
	@echo "  2. Run standalone (one-shot):"
	@echo "             actsc --help"

# ---------------------------------------------------------------
# Install systemd service unit
# ---------------------------------------------------------------
install-service: install
	@echo ""
	@echo "Installing systemd unit..."
	install -D -m 644 contrib/actsc.service \
	    $(DESTDIR)$(UNITDIR)/actsc.service
	@echo "  Installed : $(DESTDIR)$(UNITDIR)/actsc.service"
	systemctl daemon-reload
	@echo ""
	@echo "Service installed."
	@echo ""
	@echo "Enable and start:"
	@echo "  systemctl enable --now actsc"
	@echo ""
	@echo "Check status:"
	@echo "  systemctl status actsc"
	@echo ""
	@echo "Follow logs:"
	@echo "  journalctl -fu actsc"
	@echo "  tail -f $(LOGDIR)/sync"
	@echo "  tail -f $(LOGDIR)/connections"
	@echo "  tail -f $(LOGDIR)/messages"

# ---------------------------------------------------------------
# Uninstall
#
# Removes binary, unit, and logrotate.
# Preserves config and logs intentionally — sync history and
# configuration are considered operator data.
# Preserves user/group — remove manually if desired.
# ---------------------------------------------------------------
uninstall:
	@echo "Removing actsc..."
	-systemctl stop    actsc 2>/dev/null
	-systemctl disable actsc 2>/dev/null
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	rm -f $(DESTDIR)$(UNITDIR)/actsc.service
	rm -f $(DESTDIR)$(LOGROTDIR)/actsc
	-systemctl daemon-reload 2>/dev/null
	@echo ""
	@echo "Uninstall complete."
	@echo ""
	@echo "The following are preserved and must be removed manually:"
	@echo ""
	@echo "  Configuration:"
	@echo "    rm -rf $(DESTDIR)$(CONFDIR)"
	@echo ""
	@echo "  Logs:"
	@echo "    rm -rf $(DESTDIR)$(LOGDIR)"
	@echo ""
	@echo "  Service user/group:"
	@echo "    userdel  $(ACTSC_USER)"
	@echo "    groupdel $(ACTSC_GROUP)"

# ---------------------------------------------------------------
# Clean build artefacts
# ---------------------------------------------------------------
clean:
	rm -f $(TARGET)
	@echo "Cleaned"

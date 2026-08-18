CC ?= cc
PKG_CONFIG ?= pkg-config
CFLAGS ?= -O2 -Wall -Wextra -Wpedantic
SYSTEMD_CFLAGS := $(shell $(PKG_CONFIG) --cflags libsystemd)
SYSTEMD_LIBS := $(shell $(PKG_CONFIG) --libs libsystemd)

.PHONY: all clean

all: onekvm-watchdog

onekvm-watchdog: onekvm-watchdog.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SYSTEMD_CFLAGS) $(LDFLAGS) -o $@ $< $(SYSTEMD_LIBS)

clean:
	$(RM) onekvm-watchdog

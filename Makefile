CC      ?= gcc
TARGET  := kwort-systray
SRCS    := tray.c audio.c display.c network.c battery.c
OBJS    := $(SRCS:.c=.o)

PKGS    := x11 xrandr cairo cairo-xlib pangocairo pango glib-2.0 \
           libpulse libnl-3.0 libnl-genl-3.0

CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
           $(shell pkg-config --cflags $(PKGS))
LDLIBS  := $(shell pkg-config --libs $(PKGS)) -lpthread -lm

PREFIX    ?= /usr
SCRIPTDIR ?= /usr/lib/kwort
UDEVDIR   ?= /etc/udev/rules.d
# Staged installs (packaging) never need sudo: make install DESTDIR=$(pwd)/pkg
ifneq ($(strip $(DESTDIR)),)
SUDO      ?=
endif
SUDO      ?= sudo

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LDLIBS)

%.o: %.c common.h
	$(CC) $(CFLAGS) -c -o $@ $<

install: $(TARGET)
	$(SUDO) install -Dm755 $(TARGET) $(DESTDIR)$(PREFIX)/bin/$(TARGET)
ifeq ($(strip $(DESTDIR)),)
	$(SUDO) setcap "cap_net_admin+ep" $(PREFIX)/bin/$(TARGET)
else
	-$(SUDO) setcap "cap_net_admin+ep" $(DESTDIR)$(PREFIX)/bin/$(TARGET)
endif
	$(SUDO) install -Dm755 network-setup.sh $(DESTDIR)$(SCRIPTDIR)/network-setup.sh
	$(SUDO) install -Dm755 power-profile.sh $(DESTDIR)$(SCRIPTDIR)/power-profile.sh
	$(SUDO) install -Dm755 disable-display.sh $(DESTDIR)$(SCRIPTDIR)/disable-display.sh
	$(SUDO) install -Dm644 89-kwort-video.rules $(DESTDIR)$(UDEVDIR)/89-kwort-video.rules
ifeq ($(strip $(DESTDIR)),)
	-$(SUDO) udevadm control --reload-rules
	-$(SUDO) udevadm trigger --subsystem-match=backlight
endif

clean:
	rm -f $(OBJS) $(TARGET)

.PHONY: all install clean

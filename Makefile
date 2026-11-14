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
XINITRCDIR ?= /etc/X11/xinit/xinitrc.d
SUDOERSDIR ?= /etc/sudoers.d
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
	@# first install only: enable the compositor (keep a later "disable")
	@if [ ! -e $(DESTDIR)$(SCRIPTDIR)/compositor.sh ]; then \
		echo "enabling compositor: $(XINITRCDIR)/0-compositor.sh"; \
		$(SUDO) install -d $(DESTDIR)$(XINITRCDIR); \
		$(SUDO) ln -sfn $(SCRIPTDIR)/compositor.sh $(DESTDIR)$(XINITRCDIR)/0-compositor.sh; \
	fi
	$(SUDO) install -Dm755 compositor.sh $(DESTDIR)$(SCRIPTDIR)/compositor.sh
	$(SUDO) install -Dm755 display-change.sh $(DESTDIR)$(SCRIPTDIR)/display-change.sh
	-$(SUDO) rm -f $(DESTDIR)$(SCRIPTDIR)/disable-display.sh   # renamed to display-change.sh
	$(SUDO) install -Dm644 89-kwort-video.rules $(DESTDIR)$(UDEVDIR)/89-kwort-video.rules
	@# sudoers: paths follow SCRIPTDIR; refuse to install a file visudo rejects
	sed "s#/usr/lib/kwort/#$(SCRIPTDIR)/#g" kwort-systray.sudoers > kwort-systray.sudoers.out
	if command -v visudo >/dev/null 2>&1; then visudo -cf kwort-systray.sudoers.out; fi
	$(SUDO) install -Dm440 kwort-systray.sudoers.out $(DESTDIR)$(SUDOERSDIR)/kwort-systray
	rm -f kwort-systray.sudoers.out
ifeq ($(strip $(DESTDIR)),)
	-$(SUDO) udevadm control --reload-rules
	-$(SUDO) udevadm trigger --subsystem-match=backlight
endif

clean:
	rm -f $(OBJS) $(TARGET) kwort-systray.sudoers.out

.PHONY: all install clean

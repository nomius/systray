# kwort-systray — Architecture

> Updated against the source (function names, constants, files and Makefile were checked). None of the C code has been compiled or run by the assistant, which has had no C toolchain. The user builds on Linux. Scripts were tested in a sandbox with fake commands. Items marked **VERIFY** still need checking.

## 1. Application architecture

`kwort-systray` is a single-process, single-binary X11 system tray application. It creates four XEmbed tray icons: network, display, audio, battery, in that order. Clicking an icon opens a borderless popup that the application draws itself.

- **Event loop (tray.c):** `poll()` on the X connection fd plus a wake pipe, with a 1 s tick that calls each module's `tick`. Worker threads call `app_wake` to interrupt the poll.
- **Module interface (`Module` struct in common.h):** `init`, `fini`, `tick`, `icon_width`, `draw_icon`, `tooltip`, `scroll`, `middle_click`, `popup_open`, `popup_close`, `popup_size`, `popup_draw`, `popup_button`, `popup_motion`. Each module implements it; `tray.c` is the host.
- **Rendering:** Cairo xlib surfaces with push_group double buffering; Pango for text. A shared UI toolkit lives in `tray.c` and is declared in `common.h` (`ui_text`, `ui_fill`, `ui_frame`, `ui_sep`, `ui_header`, `ui_toggle`, `ui_slider`, `ui_button`, `ui_section`, …).
- **Privileged or slow work** is delegated to external scripts launched through a double-fork helper (`app_run` / `app_run_ex`), never done in the process. Privileged steps inside the scripts go through `sudo -n`, limited by the shipped sudoers file.
- **System glue** outside the process:
  - udev runs `display-change.sh` on display hotplug and sets backlight permissions;
  - xinitrc sources `compositor.sh` through a symlink;
  - a sysctl file allows unprivileged ICMP sockets.

```
                 +------------------- kwort-systray (unprivileged, cap_net_admin) -------------------+
  X server <---> | tray.c: dock, popups, tooltips, poll loop, ui_*, app_run_ex, password dialog      |
                 |   network.c   display.c   audio.c   battery.c                                     |
                 +-----|-------------|-----------|-----------|----------------------------------------+
                       |             |           |           +--> sysfs power_supply / cpufreq
     nl80211 (libnl) <-+             |           +--> PulseAudio (threaded mainloop)
     wpa_supplicant socket <---------+--> sysfs backlight, RandR, xrandr
     /proc/net/route, /sys/class/net |
           |                         |
           v app_run_ex              v app_run_ex / sh -c
   network-setup.sh  power-profile.sh  compositor.sh  display-change.sh <--- udev (89-kwort-video.rules)
           \______________ sudo -n (kwort-systray.sudoers) ____/              |
                                                                     restart panel + systray
```

## 2. Technology stack

| Area | Technology |
|---|---|
| Language | C (gnu11) |
| Windowing | Xlib, XRandR (Xlib API), XEmbed system tray protocol |
| Drawing | Cairo (cairo-xlib), Pango/PangoCairo, glib-2.0 |
| Audio | libpulse (threaded mainloop) |
| Wi-Fi | libnl-3 / libnl-genl-3 (nl80211) |
| Wi-Fi supplicant | wpa_supplicant control socket (in `network.c`); `wpa_cli` (in `network-setup.sh`) |
| Routes / links | `/proc/net/route`, `/sys/class/net/<if>/{type,tun_flags,flags,statistics}` |
| Brightness / power | sysfs (`/sys/class/backlight`, cpufreq, `platform_profile`, `power_supply`) |
| Monitor layout | RandR (read), `xrandr` command (apply) |
| Compositor | `xcompmgr` by default (`KWORT_COMPOSITOR`) |
| Panel | tint2 by default (`KWORT_PANEL`) |
| Other commands | `dhcpcd -n`, `sudo -n`, `tee`, `logger`, `ping` (fallback), `pgrep`/`pkill`, `flock`, `setsid`, `su`, `setcap`, `udevadm`, `sysctl`, `visudo` (install-time check) |
| Build | GNU Make + pkg-config; pthread, libm |
| Scripts | bash (`network-setup.sh`, `power-profile.sh`, `display-change.sh`), POSIX sh (`compositor.sh`) |

## 3. Component responsibilities

- **tray.c:**
  - icon creation and docking (`_NET_SYSTEM_TRAY_Sn`, `_NET_SYSTEM_TRAY_OPCODE`, `_XEMBED_INFO`, ParentRelative background);
  - watching the tray manager with `check_tray(int force)`: re-docks when the owner changes, on a `MANAGER` ClientMessage (forced), or when `ReparentNotify` shows an icon reparented to the root window (`redock_pending`);
  - popup lifecycle (override-redirect window, `XGrabPointer`, closes on an outside click), tooltips, event dispatch;
  - the UI toolkit, the `app_run`/`app_run_ex` launcher, and the password dialog (`app_ask_password`, wiped with `explicit_bzero`).

  `SIGCHLD` is not ignored (verified).
- **audio.c:** PulseAudio context on a threaded mainloop; a mutex-protected device cache (`Dev`); mute toggle, default output and input switching (moves streams), volume; scroll over the icon ±5 (`SCROLL_STEP`), middle-click mute; yellow icon when muted; reconnects when the server goes away.
- **display.c:**
  - brightness through sysfs backlight writes (`find_backlight`, `set_brightness`);
  - monitors through RandR (`refresh_monitors`, `Mon` records);
  - drag with snapping (`snap`), applied by `apply_layout`;
  - `set_primary`, and `disable_display` (right-click menu, `menu_mon`), which hands off the primary to `is_internal` displays first;
  - `run_xrandr(xargs, restart_panel)`, which chains `display-change.sh --restart-panel` when the primary changes;
  - the compositor menu (`cm_*`, `compositor_action`, `compositor_enabled` = `lstat(COMPOSITOR_LINK)`).
- **network.c:**
  - route and connection detection (`read_route`, `is_ethernet`, `is_vpn`, `vpn_up_iface`), IP, traffic, DNS detection (`detect_dns`);
  - nl80211 queries (interface, station, scan results; `TRIGGER_SCAN`) and encryption classification;
  - known networks through the wpa_supplicant control socket (`load_known`, with the `~/.config/systray/known_networks` fallback);
  - the ping thread (ICMP datagram socket, falls back to `ping`);
  - glyphs: `wifi_glyph`, `wired_glyph`, `vpn_badge`, chosen by `show_wired()` / `glyph_net()`;
  - the popup UI (scan, Rebind IP, DNS buttons, network lists with encryption icons and hover tooltip), the password prompt, and script invocation.

  `extend_path()` adds the sbin directories to `PATH`. `valid_ifname()` guards interface names used in paths.
- **battery.c:** reads `/sys/class/power_supply` batteries (`Bat`, up to `MAXBAT` 8; energy- or charge-based), computes the overall %, state, per-battery lines, the charge threshold, and the current power profile from sysfs. The profile buttons run `power-profile.sh`. `init` returns -1 without a battery, so there is no icon.
- **network-setup.sh:** modes `WIFI`, `REBIND`, `DNS` (§5.5).
- **power-profile.sh:** applies or reads the power profile through sysfs for all CPUs and platform_profile.
- **display-change.sh:** udev hotplug handler and the shared `--restart-panel` routine (§5.6).
- **compositor.sh:** compositor start/stop/restart/enable/disable/status; also sourced by xinitrc (§5.7).
- **89-kwort-video.rules:** backlight → group `video`, g+w; a DRM `change` on `card0` → `display-change.sh`.
- **kwort-systray.sudoers:** narrow NOPASSWD rules for `%users` (§6).
- **ping.conf:** `net.ipv4.ping_group_range=0 2147483647`.

## 4. Data models (in memory)

- `Rgba` colour type and colour constants (`C_BG`, `C_FG`, `C_DIM`, `C_ACCENT`, `C_ROW`, `C_LINE`, `C_ICON`, `C_RED`, `C_YELLOW`, `C_BLUE`, `C_FILL`).
- `Module`: function-pointer table (§1).
- network.c:
  - State `N`: `iface`, `ip`, `gw`, `up` (default route present), `wifi` (iface is wireless), `ether` (iface is Ethernet, not tun/tap), `vpn[32]` (VPN interface on top, or ""), `ssid`, `freq`, `dbm`, `have_dbm`, rx/tx rate and totals, `dns`, `no_ns`, `aps[MAXAP]`, `nap`.
  - `Ap`: `ssid`, `bssid[18]`, `dbm`, `freq`, `enc`, `connected`, `known`.
  - Encryption enum `ENC_OPEN, ENC_WEP, ENC_WPA, ENC_WPA2, ENC_WPA3`, with `enc_arg[]` (script argument strings) and `enc_label[]` (tooltip text).
  - Known-network list (`known[MAXKNOWN][64]`, `nknown`). `MAXKNOWN` = 32, `MAXAP` = 32.
  - Pending-connection state for the password dialog (`pending_ssid`, `pending_bssid`, `pending_enc`); hover state.
  - Layout struct `Lay`.
- audio.c: device cache (`Dev`) guarded by a mutex.
- display.c: `Mon { name[32], active, primary, x, y, w, h }`, `mons[MAXMON]`, layout `Lay`, menu state (`menu_mon`), compositor menu state (`cm_open`, `cm_hover`, `cm_item`).
- battery.c: `Bat { name[32], path[320], full_wh, now_wh, pct, status[24] }`, `bats[MAXBAT]`.

## 5. APIs and integrations

### 5.1 X11
- XEmbed docking.
- Override-redirect popup windows with a pointer grab (`owner_events=True`). The password dialog also grabs the keyboard.
- `MANAGER` ClientMessage and `ReparentNotify` handling for re-docking.

### 5.2 PulseAudio
Threaded mainloop; introspection of sinks and sources; default-device switching with stream moves; subscriptions for live updates.

### 5.3 nl80211 through libnl
Dump commands `GET_INTERFACE`, `GET_STATION`, `GET_SCAN`, plus `TRIGGER_SCAN` (needs `CAP_NET_ADMIN`). Encryption is classified from BSS information elements:
- RSN IE (id 48): AKM suites → PSK / SAE / EAP / OWE.
- WPA vendor IE (id 221, 00:50:F2:01) → WPA.
- Capability privacy bit (0x10) without WPA/RSN → WEP.
- Classification: OWE-only RSN → open; SAE without PSK → WPA3; other RSN → WPA2; WPA IE → WPA; privacy bit → WEP; otherwise open.

### 5.4 Routes, connection type and VPN (network.c `read_route`)
- Parses `/proc/net/route`: Iface, Destination, Gateway, Flags, RefCnt, Use, Metric, Mask. Addresses are raw network-order words, so `/1` masks are compared with `htonl(0x80000000)`.
- **Physical link:** the lowest-metric route that is up (`RTF_UP`) with `RTF_GATEWAY`, destination 0, mask 0, on an interface that is not a VPN.
- **VPN interface (`is_vpn`):** has `tun_flags`, or `type` = 65534. It counts when it holds a default route (no gateway needed) or the OpenVPN `def1` routes. Otherwise `vpn_up_iface()` looks for any VPN interface with `IFF_UP` in `/sys/class/net/*/flags` (for wg-quick policy routing). Names of `IFNAMSIZ` or longer are skipped, and names are copied with `%.15s`.
- If only a VPN default route exists, `N.iface` is the VPN interface.
- `N.ether = is_ethernet(iface)`: `type` = 1 and no `tun_flags`.
- `show_wired()` = up ? `ether && !wifi` : no wireless interface present.
- Ping goes to the physical gateway. DHCP rebind and `DNS dhcp` use `N.iface`, the physical link.

### 5.5 wpa_supplicant control socket (network.c)
- UNIX datagram socket. The client binds `/tmp/systray_wpa_<pid>_<n>`, connects to `<dir>/<iface>`, sends `LIST_NETWORKS`, uses a 2 s receive timeout and skips unsolicited `<…>` messages.
- `<dir>` = `$SYSTRAY_WPA_DIR`, default `/var/run/wpa_supplicant`.
- Fallback when unusable: `~/.config/systray/known_networks` (one SSID per line).

### 5.6 External scripts (invoked with `app_run` / `app_run_ex`)

| Script | Default path (common.h) | Override |
|---|---|---|
| network-setup.sh | `/usr/lib/kwort/network-setup.sh` | `SYSTRAY_NET_SCRIPT` |
| power-profile.sh | `/usr/lib/kwort/power-profile.sh` | `SYSTRAY_POWER_SCRIPT` |
| display-change.sh | `/usr/lib/kwort/display-change.sh` | `SYSTRAY_DISPLAY_SCRIPT` |
| compositor.sh | `/usr/lib/kwort/compositor.sh` | `SYSTRAY_COMPOSITOR_SCRIPT` |

`COMPOSITOR_LINK` = `/etc/X11/xinit/xinitrc.d/0-compositor.sh`.

- `network-setup.sh WIFI <interface> <essid> <address> <open|wep|wpa|wpa2|wpa3>`:
  - the secret is in the `WIFI_PASSWORD` environment variable;
  - uses `wpa_cli` (add/set/enable/select/save) with WEP, WPA (proto WPA), WPA2 (proto RSN), WPA3 (SAE, `ieee80211w 2`) and open (`key_mgmt NONE`) handling;
  - passwords must be 8–63 characters for WPA\*; WEP keys 5/13 ASCII characters or 10/26 hex digits;
  - a failed new secured network is removed.
- `network-setup.sh REBIND <interface>`: validates `^[A-Za-z0-9][A-Za-z0-9._-]*$`. When not root it runs `sudo -n -- <self> REBIND <iface>`; as root `dhcpcd -n -- <iface>`.
- `network-setup.sh DNS <dhcp|cloudflare|google|custom> [interface]`:
  - **dhcp** with an interface runs the REBIND path; without one nothing changes;
  - otherwise writes `resolv.conf` (directly if writable, else `sudo -n tee`) and keeps search/domain/options;
  - cloudflare 1.1.1.1/1.0.0.1, google 8.8.8.8/8.8.4.4, custom 127.0.0.1.
- `power-profile.sh <Power-saver|Balanced|Performance|get>`: governor and EPP per CPU (cpufreq), plus `/sys/firmware/acpi/platform_profile`. Re-runs itself as `sudo -n -- <self> <profile>` when not root.
- `display-change.sh --restart-panel` is called from `run_xrandr` as `sh -c 'panel=$1; shift; xrandr "$@" || exit; exec "$panel" --restart-panel'`, so the restart happens only if xrandr succeeded and survives the systray being killed.
- `compositor.sh restart|stop|disable|enable` comes from the display popup menu.

### 5.7 display-change.sh (udev and panel restart)
- **udev mode** (no arguments):
  - detaches (`setsid bash "$0" --detached`), sleeps 2 s, and takes `flock` on `/run/kwort-display.lock` (fd 9);
  - finds the session through `pgrep -o -x kwort-systray || pgrep -o -x $PANEL_NAME` and imports `DISPLAY`, `XAUTHORITY`, `XDG_RUNTIME_DIR` and `KWORT_PANEL` from `/proc/<pid>/environ`. Root can read kwort-systray's environment despite setcap. The fallback is `who` with `:0` and `~/.Xauthority`;
  - `query()` parses `xrandr --query` into name, connected, primary and active, and compares the result with `/run/kwort-display.state`;
  - disconnected outputs that are still active get `--off` plus a primary handoff; new outputs get the off, sleep 1, `--auto --right-of <primary>` jumpstart;
  - then runs `as_user env KWORT_PANEL=… bash <self> --restart-panel` (`su -s /bin/bash <user> -c …`).
- **`--restart-panel`** (as the desktop user):
  - lock `/run/user/<uid>/kwort-panel.lock` (fd 8, `flock -w 30`; TMPDIR or `/tmp` fallback);
  - collects the panel and kwort-systray PIDs and the command line and environment of the oldest of each from `/proc`. The systray's environment is unreadable because of setcap, so the panel's is used;
  - SIGTERM to all, waits up to 3 s, then SIGKILL;
  - starts the panel (`setsid env -i <env> <cmd> 8>&- 9>&-`) unless one was respawned, sleeps 1, then starts the systray the same way.
- `KWORT_PANEL=${KWORT_PANEL:-tint2}`; `PANEL_NAME` is the basename of its first word.

### 5.8 compositor.sh
- `KWORT_COMPOSITOR=${KWORT_COMPOSITOR:-"xcompmgr -c -f -C -r 5 -o .6 -D 5"}`. The process name is the basename of its first word (`pgrep -x -u $UID`).
- **Sourced mode:** `$0` doesn't end in `compositor.sh` (xinitrc sources `xinitrc.d/*`). It backgrounds the compositor if installed, ignores arguments, never exits, and unsets its own flag variable.
- **Executed mode:** `start` (does nothing if running, `setsid`), `stop` (pkill, up to 2 s, then KILL), `restart`, `disable` = `set_link off` + stop, `enable` = `set_link on` + start, `status`, and the internal `--link on|off` (root only).
- `set_link` as non-root → `sudo -n <self> --link on|off`.

### 5.9 Other
ICMP datagram socket for ping (needs `ping_group_range`); `ping -n -c 1 -W 1 <gw>` as the fallback through `popen` (the only `popen` in `network.c`).

## 6. Authentication and authorization

The app has no user authentication. Authorization relies on OS mechanisms:
- **File capability:** `setcap "cap_net_admin+ep"` on the binary, for the Wi-Fi scan trigger. It also makes the process non-dumpable, so its `/proc/<pid>/environ` is readable only by root, which `display-change.sh` works around.
- **sudo:** `kwort-systray.sudoers` → `/etc/sudoers.d/kwort-systray`, mode 0440, must be owned by root.
  - `User_Alias KWORT_USERS = %users`.
  - `KWORT_POWER`: `power-profile.sh power-saver|balanced|performance`.
  - `KWORT_COMPOSITOR`: `compositor.sh --link on|off`.
  - `KWORT_NETWORK`: `/usr/bin/tee /etc/resolv.conf`, `/bin/tee /etc/resolv.conf`, `network-setup.sh REBIND *`.
  - `Defaults!KWORT_CMDS !requiretty`; `KWORT_USERS ALL = (root) NOPASSWD: KWORT_CMDS`.
  - dhcpcd is not allowed directly, because `-c <script>` would run code as root. REBIND validates its argument as root before running dhcpcd.
- **udev rules:** the backlight `brightness` file belongs to group `video` with g+w. The display hotplug handler runs as root and drops to the desktop user with `su` for xrandr and the panel restart.
- **sysctl:** `ping_group_range` lets every group open ICMP datagram sockets.
- **wpa_supplicant socket:** needs access to the socket, usually through the group in `ctrl_interface … GROUP=` (not verified on the user's system).
- **Wi-Fi secrets:** collected in an in-app dialog and passed to the script only through `WIFI_PASSWORD`, never as an argument. The dialog wipes the buffer (`explicit_bzero`), and the log masks environment values.

## 7. Deployment architecture

Native Linux install with `make install`:
- `$(DESTDIR)$(PREFIX)/bin/kwort-systray` (`PREFIX` default `/usr`), then `setcap` (fatal for a real install, tolerated with `DESTDIR`).
- `$(DESTDIR)$(SCRIPTDIR)/{network-setup,power-profile,compositor,display-change}.sh` (755; `SCRIPTDIR` default `/usr/lib/kwort`). The obsolete `disable-display.sh` is removed.
- `$(DESTDIR)$(XINITRCDIR)/0-compositor.sh` → `$(SCRIPTDIR)/compositor.sh`, created only when `compositor.sh` wasn't installed yet (first install).
- `$(DESTDIR)$(UDEVDIR)/89-kwort-video.rules` (644; default `/etc/udev/rules.d`).
- `$(DESTDIR)$(SYSCTLDIR)/ping.conf` (644; default `/etc/sysctl.d`).
- `$(DESTDIR)$(SUDOERSDIR)/kwort-systray` (440; default `/etc/sudoers.d`): paths are rewritten with sed to `SCRIPTDIR` and checked with `visudo -cf` when available, so a rejected file isn't installed.
- Only without `DESTDIR`: `sysctl -p $(SYSCTLDIR)/ping.conf`, `udevadm control --reload-rules`, `udevadm trigger --subsystem-match=backlight` (all allowed to fail).
- `SUDO` defaults to `sudo` and is empty when `DESTDIR` is set (packaging). A package must apply setcap, root ownership of the sudoers file, `sysctl` and the udev reload in its post-install step.

Runtime: started from the user's X session after the panel (e.g. `kwort-systray &` in `~/.xinitrc`). The compositor starts from `xinitrc.d`. tint2 and kwort-systray may be restarted by `display-change.sh`.

## 8. Important technical constraints

- Linux with X11 and a running system tray (XEmbed manager). No Wayland.
- The build requires the pkg-config packages x11, xrandr, cairo, cairo-xlib, pangocairo, pango, glib-2.0, libpulse, libnl-3.0 and libnl-genl-3.0.
- `_GNU_SOURCE` is guarded with `#ifndef` in each `.c` (not set in the Makefile).
- Warning-clean with `-Wall -Wextra`. `-Wformat-truncation` needs bounded `%.Ns` (e.g. `%.15s` for `d_name` → interface buffers).
- `SIGCHLD` must not be `SIG_IGN` (it breaks `pclose`). Children are launched by double fork.
- The user's unprivileged `PATH` lacks sbin. `extend_path()` and the scripts add `/sbin` and `/usr/sbin`. `wpa_cli` is `/usr/sbin/wpa_cli` on the user's machine.
- `ui_text` calls `cairo_new_path` after drawing (otherwise a stray line appears).
- The Wi-Fi scan trigger needs `CAP_NET_ADMIN`.
- Processes started by the scripts must not inherit lock fds (`8>&- 9>&-`). An inherited lock caused a hang earlier.
- tint2 re-executes itself on a compositor change and keeps the same tray owner window ID, so re-docking must not depend only on an owner change.
- Script mode bits: `.sh` files are 755 in git and installed 755. `display-change.sh` calls itself with `bash "$0"`, so it doesn't depend on the mode.
- WEP may be unsupported by the installed wpa_supplicant build. Enterprise (EAP) networks can't be set up through the password-only flow.
- The password dialog is ASCII only, with no paste.
- Fixed-size arrays: 32 APs, 32 known networks, 64-character SSIDs in the known list, an 8 KiB control-socket reply buffer, 8 batteries, `MAXMON` monitors.

## 9. Items requiring verification

1. Runtime behaviour of the code that hasn't been compiled: the latest `vpn_up_iface()` warning fix, the VPN badge rendering at tray size, and the Ethernet glyph appearance.
2. `display-change.sh` and `compositor.sh` on the real system (they were tested only with fake `pgrep`/`xrandr`/`sudo`), and the sudoers file under real `visudo`/sudo.
3. That `dhcpcd -n -- <iface>` is accepted by the installed dhcpcd.
4. The wpa_supplicant control group on Kwort.

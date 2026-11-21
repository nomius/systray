# CLAUDE.md — kwort-systray

## 1. Project overview
`kwort-systray` is a single-binary Linux X11 system-tray app in C for the Kwort desktop. It shows four XEmbed icons, in this order: **network, display, audio, battery**. Each icon opens a self-drawn borderless popup. Privileged or system-level work goes to helper scripts: DNS, Wi-Fi setup, DHCP rebind, power profiles, compositor control, display hotplug and panel restarts.

Files:
- `tray.c`: host. Docking (and re-docking after a tray restart), popups, tooltips, event loop, UI toolkit, `app_run_ex`, password dialog.
- `common.h`: `Module` interface, colours, UI helper declarations, script path defaults, `COMPOSITOR_LINK`.
- `audio.c`, `display.c`, `network.c`, `battery.c`: one `Module` each.
- Helper scripts, installed to `/usr/lib/kwort/`:
  - `network-setup.sh` (bash): Wi-Fi, DNS, DHCP rebind.
  - `power-profile.sh` (bash): sysfs power profiles.
  - `display-change.sh` (bash): udev display hotplug handler and the shared `--restart-panel` routine (panel + systray).
  - `compositor.sh` (POSIX sh): start/stop/restart/enable/disable the compositor. Also sourced by xinitrc through the symlink `/etc/X11/xinit/xinitrc.d/0-compositor.sh`.
- `89-kwort-video.rules`: udev rules: backlight → group `video`, and DRM change on `card0` → `display-change.sh`. User-owned; read it before changing.
- `kwort-systray.sudoers`: narrow NOPASSWD rules for the scripts' `sudo -n` calls, installed to `/etc/sudoers.d/kwort-systray`.
- `ping.conf`: `net.ipv4.ping_group_range` for the unprivileged ICMP ping socket, installed to `/etc/sysctl.d/`. Added by the user.
- `Makefile`, `README.md` (user-facing documentation; screenshots in `screenshots/`).
- `docs/REQUIREMENTS.md`, `docs/ARCHITECTURE.md`, `docs/DECISIONS.md`: specifications. Keep them in sync when behaviour changes, and verify them against the source.

## 2. Technology stack
- C (gnu11), Xlib and XRandR, Cairo (cairo-xlib) and Pango.
- libpulse (threaded mainloop), libnl-3 and libnl-genl-3 (nl80211), pthread.
- sysfs for brightness, power supply, cpufreq, platform_profile and network links (`/sys/class/net/<if>/{type,tun_flags,flags,statistics}`). `/proc/net/route` for routes.
- wpa_supplicant control socket in `network.c`.
- Tools used in the scripts or as fallbacks: `wpa_cli`, `dhcpcd`, `xrandr`, `sudo -n`, `tee`, `logger`, `ping`, `pgrep`/`pkill`, `flock`, `setsid`, `su`.
- Default panel: tint2. Default compositor: xcompmgr.
- GNU Make and pkg-config (packages listed in the `Makefile`).

## 3. Architecture principles
- One `Module` struct per widget (see `common.h`). Add behaviour inside a module; keep `tray.c` generic.
- The UI thread is the `poll()` loop on the X fd plus a wake pipe, with a 1 s tick. Never block it.
- Background threads (ping, PulseAudio) update mutex-protected state and call `app_wake()`.
- The tray process stays unprivileged. Privileged operations go through the scripts (`sudo -n`, limited by the sudoers file), file capabilities, sysctl or udev. Do not add in-process root logic.
- Prefer sysfs, sockets and netlink over spawning tools. Do not add new external commands without asking the user.
- Reuse the `ui_*` helpers for all drawing. Do not duplicate drawing code.
- Panel restarts go only through `display-change.sh --restart-panel`, which holds a per-user lock and never leaves two panels or systrays. Don't add another restart path.
- Environment overrides:
  - kwort-systray: `SYSTRAY_NET_SCRIPT`, `SYSTRAY_POWER_SCRIPT`, `SYSTRAY_DISPLAY_SCRIPT`, `SYSTRAY_COMPOSITOR_SCRIPT`, `SYSTRAY_WPA_DIR` (default `/var/run/wpa_supplicant`).
  - Scripts: `KWORT_PANEL` (default `tint2`) and `KWORT_COMPOSITOR` (default `xcompmgr -c -f -C -r 5 -o .6 -D 5`), set as `${VAR:-default}` so the environment wins.

## 4. Coding conventions
- Match the surrounding style: C99/gnu11, 4-space indent, `static` for module-private symbols, short comments only where the reason is not obvious.
- Keep `#ifndef _GNU_SOURCE / #define _GNU_SOURCE / #endif` at the top of each `.c`. Do not define it in the `Makefile`.
- The build must stay warning-clean with `-Wall -Wextra`. Use bounded `snprintf` with `%.Ns` precisions and adequately sized buffers (`-Wformat-truncation`). For example, interface names from `readdir` are copied with `%.15s` after skipping names of `IFNAMSIZ` or longer.
- After drawing text with Cairo or Pango, call `cairo_new_path` (the `ui_text` helper already does).
- Bash scripts: `set -u` (where the script already uses it), quote everything, extend `PATH` with `/sbin:/usr/sbin`, log through the script's `log()`. `compositor.sh` is POSIX sh, because xinitrc sources it: it must never `exit` or block in sourced mode.
- Started background processes must not inherit lock fds (`8>&- 9>&-` in `display-change.sh`).
- Edit with exact, minimal diffs. Preserve the user's own edits (for example `logger` in `log()`).

## 5. Development workflow
- The assistant's environment may have no C toolchain. It has been a Windows machine, or a Linux sandbox without gcc, make, visudo or shellcheck. Then **the C code cannot be compiled or run**. Say so, and ask the user to build on Linux and paste the compiler or runtime output. Scripts can still be checked with `bash -n` / `sh -n` and exercised with fake commands on `PATH`.
- On Linux:
  ```bash
  make            # builds kwort-systray
  make clean
  make install    # real install: needs sudo (setcap, /usr/lib/kwort, udev, sysctl, sudoers)
  make install DESTDIR=$PWD/pkg   # staged, no sudo, does not touch the running system
  ```
- Use `SYSTRAY_*` env vars to run against a local copy of the scripts without installing.
- Packaging must keep working: respect `DESTDIR`, `PREFIX` (default `/usr`), `SCRIPTDIR` (default `/usr/lib/kwort`), `UDEVDIR`, `SYSCTLDIR`, `XINITRCDIR`, `SUDOERSDIR`. Steps that change the running system (`sysctl -p`, udev reload) belong in the `ifeq ($(strip $(DESTDIR)),)` block.
- The sudoers file is rewritten to `SCRIPTDIR` with sed and checked with `visudo -cf` before installing. Keep its paths in sync with the scripts' `sudo -n` calls.
- Keep the binary name `kwort-systray`.
- When behaviour changes, also update `README.md` and the `docs/` files.

## 6. Testing requirements
There is no automated test suite. Before declaring work done:
- Compile with no warnings (Linux only).
- For network features: check the scan, a known network, an open network and an unknown secured network (password dialog), the encryption icons and tooltip, the DNS buttons and Rebind IP. Check the icon for Wi-Fi, wired (real Ethernet), offline, and with a VPN on top (Wi-Fi signal plus the "V" badge).
- For display features: drag and apply, set primary (the panel restarts), right-click → Disable display (refused for the last display), plug and unplug a monitor (udev → `display-change.sh`), and the compositor menu (restart, stop, disable/enable and the symlink). Check that the tray icons come back after a compositor restart and that there's only one tint2 and one systray afterwards.
- For scripts: run `bash -n` / `sh -n` and `shellcheck` when available, then try the modes by hand with fake commands (`wpa_cli`, `sudo`, `pgrep`, `xrandr`, `dhcpcd`) or a safe interface.
- Check audio mute, scroll, and device switching; battery with one and with several batteries; all three power profiles.
- Check that a popup closes on an outside click, and check the tooltips.
- Report untested code as untested.

## 7. Security requirements
- **Wi-Fi secrets only in the environment variable `WIFI_PASSWORD`, never as an argument.** Never log them. Logs may show the env var name but must mask the value. Do not mask the ESSID.
- Wipe password buffers (`explicit_bzero`) after use.
- Validate interface names before using them in paths or commands (`valid_ifname` in C). In `network-setup.sh`, names must match `^[A-Za-z0-9][A-Za-z0-9._-]*$`: no leading `-`, which a tool would read as an option. Pass `--` before operands. Never build shell command strings from untrusted data. Use argv-style `app_run_ex`.
- Scripts use `sudo -n` (non-interactive). Never add prompts for sudo.
- Sudoers rules allow **one exact command line** each. Where an argument must vary, the script re-runs itself as root and validates the argument (e.g. `network-setup.sh REBIND *`). Never allow a tool directly whose options can run code as root (e.g. `dhcpcd`, because of `-c <script>`). The sudoers file is 0440 and must be owned by root.
- `network-setup.sh` validates its arguments. WPA\* passwords must be 8–63 characters. A new secured network without a password must be removed again.
- The binary gets only `cap_net_admin+ep` (needed for the nl80211 scan trigger). Do not widen capabilities. Ping uses the unprivileged ICMP socket (`ping.conf`), not `CAP_NET_RAW`.

## 8. Important constraints
- Linux and X11 with an XEmbed tray; no Wayland.
- Popups are override-redirect and use `XGrabPointer(owner_events=True)`. A click outside closes them.
- Never set `signal(SIGCHLD, SIG_IGN)`: it breaks `pclose`. Helpers are launched by double fork in `app_run_ex`.
- The user's PATH lacks sbin dirs. `network.c` calls `extend_path()`. Scripts add `/sbin` and `/usr/sbin`.
- Brightness uses `/sys/class/backlight/<dev>/brightness` directly. Power profiles use sysfs (platform_profile, cpufreq governor and EPP) and are applied to **all** CPUs.
- Re-docking: `check_tray(force)` must re-dock on a `MANAGER` ClientMessage and on `ReparentNotify` to the root window. tint2 re-executes itself with the same tray owner XID, so an owner comparison alone isn't enough.
- Network connection type: the described link is the lowest-metric IPv4 default route that is **not** a VPN.
  - Wired (Ethernet glyph) only for real Ethernet: `type` 1, no `tun_flags`, not wireless.
  - A VPN (tun/WireGuard type 65534 or `tun_flags`; a default route, OpenVPN `def1`, or simply up) keeps the Wi-Fi glyph of the link underneath and adds the yellow "V" badge.
- Script contracts:
  - `network-setup.sh WIFI <interface> <essid> <address> <open|wep|wpa|wpa2|wpa3>` (secret via env)
  - `network-setup.sh REBIND <interface>` (re-runs itself through `sudo -n` and then runs `dhcpcd -n -- <interface>`)
  - `network-setup.sh DNS <dhcp|cloudflare|google|custom> [interface]` (dhcp + interface = renew the lease)
  - `power-profile.sh <Power-saver|Balanced|Performance|get>`
  - `compositor.sh [start]|stop|restart|enable|disable|status` (internal: `--link on|off`, root only)
  - `display-change.sh` (no arguments: udev hotplug) and `display-change.sh --restart-panel` (as the desktop user)
- Make install creates the compositor symlink only on the first install, so a user's *disable* survives reinstalls.
- Icon colours: gray by default; network blue (red when down) with the Wi-Fi or Ethernet glyph and a yellow "V" badge for a VPN; audio yellow when muted; battery with % text.
- Fixed-size arrays (32 APs, 32 known networks, 8 KiB wpa reply, 8 batteries): check bounds when changing them.

## 9. Known pitfalls
- Wi-Fi scanning needs `CAP_NET_ADMIN`; without `setcap`, scans silently fail. setcap also makes the process non-dumpable, so its `/proc/<pid>/environ` is unreadable by the user (`--restart-panel` uses the panel's environment instead; root can still read it).
- The ICMP ping socket needs `net.ipv4.ping_group_range` (`ping.conf`); otherwise it falls back to `ping(8)`.
- The wpa_supplicant socket needs the right group, and the client binds a temp socket under `/tmp`. The socket dir comes from `SYSTRAY_WPA_DIR`.
- WEP is often compiled out of wpa_supplicant. 802.1X/EAP networks are classified as WPA2 and cannot be set up through the password dialog.
- WPA2/WPA3 mixed networks are shown as WPA2.
- SSIDs from the control socket are not unescaped (`\xNN`).
- The password dialog is ASCII-only, with no paste.
- udev runs `display-change.sh` as root without the session environment. It gets `DISPLAY`, `XAUTHORITY` and `KWORT_PANEL` from the running kwort-systray or panel, and `su` may drop variables, so pass them explicitly.
- `/proc/net/route` addresses are raw network-order words: compare masks with `htonl(...)`. wg-quick's policy routing doesn't appear there.
- PowerShell on Windows: `R` is an alias (`Invoke-History`); `Set-Content` adds a BOM or ANSI encoding. Use the Edit and Write tools for source files and avoid BOMs in `.sh` and `.c`.
- Keep header comments (e.g. the script calls listed at the top of `network.c`) in sync with the code.
- The `Makefile` runs `setcap`, `sysctl` and `udevadm`, and writes to system paths with `sudo`. Do not run `make install` without the user's go-ahead.

## 10. Instructions for modifying existing functionality
1. Read the relevant module and `common.h` before editing. Don't rely on the docs alone.
2. Check callers when changing a function signature, a script argument contract or a `Module` field. Update the C caller and the script together, along with the header comments, the usage text, the sudoers file (if a `sudo -n` call changes), `README.md` and `docs/`.
3. Keep behaviours the user explicitly asked for (see §7 and §8). Don't reintroduce `brightnessctl`, `powerprofilesctl`, or `wpa_cli` calls inside `network.c`.
4. Add new tools or dependencies only after asking. If one is needed, say what and why.
5. If an edit adds a feature that needs a library, update `PKGS` in the `Makefile`.
6. State what was changed, what was not compiled or tested, and any caveats. Ask the user for compile output when you couldn't build.

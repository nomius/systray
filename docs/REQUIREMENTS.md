# kwort-systray — Requirements

Status legend:
- **[Implemented]**: the code is written.
- **[User-verified]**: the user built and ran it and confirmed it works.
- **[Untested]**: not compiled or run by the assistant. The assistant's environment has never had a C toolchain: first a Windows machine, later a Linux sandbox without gcc, make, visudo or shellcheck. The user builds and runs on Linux and pastes the output back.
- **[Uncertain]**: an assumption or an ambiguity.
- **[Planned]**: not implemented.

> Scripts were syntax-checked (`bash -n` / `sh -n`) and exercised in the sandbox with fake commands (`pgrep`, `sudo`, `logger`, `dhcpcd`, `xrandr`). The C code is verified only by the user's builds and runs. The user accepted the first delivery and later reported compile warnings and runtime issues, which were fixed. The latest network changes (wired and VPN) were built by the user. The single warning they reported was fixed afterwards, and that fix has not been compiled.

---

## 1. Application objectives

A Linux system tray application for the Kwort desktop, written in **C** with **Xlib, Cairo and Pango**. It shows four XEmbed tray icons, each with a popup widget:

| Order | Module | Purpose |
|---|---|---|
| 1 | network | connection status (Wi-Fi, wired, VPN), Wi-Fi selection, DNS, gateway health |
| 2 | display | brightness, monitor arrangement, primary display, disabling displays, compositor management |
| 3 | audio | PulseAudio volume, mute and device control |
| 4 | battery | battery status and power profiles |

Source layout:
- `tray.c` (docking, popups, tooltips, UI toolkit), `audio.c`, `display.c`, `network.c`, `battery.c`, `common.h`.
- Helper scripts: `network-setup.sh`, `power-profile.sh`, `display-change.sh`, `compositor.sh`.
- System configuration: `89-kwort-video.rules` (udev), `kwort-systray.sudoers`, `ping.conf` (sysctl).
- `Makefile`, `README.md` with `screenshots/`, and `docs/`.

The design followed four mockups supplied by the user. The Wi-Fi band toggle and the header toggle shown in them were deliberately left out (**[Uncertain]** whether the user wants them later).

## 2. Functional requirements

### 2.1 Tray / general (tray.c) [Implemented]
- Icons dock through the XEmbed system tray protocol (`_NET_SYSTEM_TRAY_Sn`, `_NET_SYSTEM_TRAY_OPCODE`, `_XEMBED_INFO`, ParentRelative background), in the order network, display, audio, battery.
- The default icon colour is gray.
- Popups are borderless (override-redirect) and close on a click outside (pointer grab).
- Icons have tooltips.
- Main loop: `poll()` on the X connection plus a wake pipe, with a 1 s tick.
- **Re-docking after a tray restart [User-verified]:** `check_tray(int force)` docks the icons again when:
  - the tray owner changes;
  - a `MANAGER` ClientMessage arrives (forced). tint2 re-executes itself after a compositor change and gets the same owner window ID back, so comparing owners alone missed it;
  - an icon is reparented to the root window (`ReparentNotify` sets `redock_pending`).

  It logs "tray manager 0x… restarted|appeared: re-docking icons". The user confirmed: "It worked, icons come back now".
- Password dialog helper `app_ask_password`: 420×190, masked bullets, Enter/Esc, a click outside cancels, keyboard and pointer grab, the secret is wiped when done. ASCII only, no paste.
- `app_run` / `app_run_ex` launch helper scripts by double fork. They log the executable path and arguments (the ESSID is **not** masked) and the environment variable name (value masked), and warn when the file is missing or not executable.
- Script paths have defaults in `common.h` and can be overridden with environment variables (§6).

### 2.2 Audio (audio.c) [Implemented]
- PulseAudio widget: mute toggle, output device selection, input device selection, volumes.
- Scrolling over the icon changes the output volume by ±5%. A middle click toggles mute.
- Switching the default device also moves existing streams.
- When muted, the **audio icon turns yellow**.
- Reconnects when the PulseAudio server goes away.

### 2.3 Display (display.c) [Implemented]
- Brightness is read and written through `/sys/class/backlight/<dev>/brightness` **directly** (errors go to stderr). The slider also reacts to the scroll wheel (±5%). `brightnessctl` is not used.
- Monitors are discovered through RandR. Arrangement is by drag and drop, snapping to the nearest position without overlap. The layout is applied with `xrandr --output … --pos …`.
- **Set primary:** left-click a row under Displays. Nothing happens if the display is already primary.
- **Disable display:** right-click a row under Displays to open a "Disable display" menu (`MENU_W` 150, `MENU_H` 30). `disable_display()`:
  - refuses to disable the last active display;
  - if the target is the primary, moves the primary to the internal panel (eDP/LVDS, `is_internal`), otherwise to another active display;
  - turns the panel on with `--auto --right-of` if it is inactive.

  All of this is a single xrandr call.
- **Panel restart:** whenever the primary changes (set primary, or disabling the primary), tint2 and the systray are restarted so the panel follows. `run_xrandr(xargs, restart_panel)` runs `sh -c 'xrandr "$@" || exit; exec <display-change.sh> --restart-panel'` detached.
- **Compositor management [Implemented]:** a header button at the top right, "Compositor management ▾".
  - Its tooltip is "Seeing UI glitches?", shown when hovered with the menu closed.
  - Menu items: *Restart compositor*, *Stop compositor*, *Disable/Enable compositor*. The last one depends on whether `COMPOSITOR_LINK` exists (`lstat`).
  - Each item runs `compositor.sh restart|stop|disable|enable`.
- Display hotplug is handled by `display-change.sh`, which udev runs (§2.6).

### 2.4 Network (network.c) [Implemented]
- **Icon:** **blue**, or **red when the network is down**. The Wi-Fi glyph is drawn 2 px higher than other icons.
- **Connection type [Implemented, user-built]:**
  - The described link is the **lowest-metric IPv4 default route that is not a VPN**.
  - **Wired:** that interface is real Ethernet (`/sys/class/net/<if>/type` = 1, not Wi-Fi according to nl80211, and no `tun_flags`). The tray icon and the popup header then show an **Ethernet (RJ45) port** glyph. Offline with no wireless interface at all, the red Ethernet glyph is shown.
  - Otherwise the Wi-Fi glyph with signal bars is shown.
  - Text labels: "Wired" / "Wired connection" / "`<iface>` (wired)" only for real Ethernet. Other links that aren't Wi-Fi (PPP, or a VPN-only route) say "Connected".
- **VPN [Implemented, user-built]:**
  - A VPN is a tun or WireGuard interface (type 65534) or any TUN/TAP device (`tun_flags`).
  - It counts as active when it holds:
    - a default route (no gateway required), or
    - OpenVPN's `def1` routes (`0.0.0.0/1` + `128.0.0.0/1`), or
    - when no such route is visible (wg-quick uses policy routing), when a VPN interface is administratively up.
  - The widget keeps describing the physical link underneath (Wi-Fi signal, ping, traffic, IP).
  - It adds a **yellow "V" badge** to the bottom right of the tray icon and the header glyph, "· VPN" to the header subtitle, and "— VPN `<iface>`" to the tooltip.
  - If only a VPN default route exists, the VPN interface itself is described.
- Tooltip: the SSID, "`<iface>` (wired)", "`<iface>`", or "No network", plus the VPN suffix.
- **Wi-Fi info and scan** through libnl-3/nl80211: SSID, band, signal in dBm with a quality label in parentheses, and a list of scan results.
- **Signal quality labels:** **Excellent** ≥ −66, **Good** −67..−70, **Fair** −71..−80, **Unusable** ≤ −81 dBm.
- The **Scan** button triggers a scan (needs `CAP_NET_ADMIN`) so another wireless network can be chosen.
- The **Rebind IP** button, next to Scan, runs `network-setup.sh REBIND <iface>`.
- **DNS provider buttons:** DHCP, Cloudflare, Google, Custom. The Custom button contains a "(?)" help marker explaining that Custom points DNS to localhost so a local resolver can be used. DHCP passes the interface: `DNS dhcp <iface>`.
- **DNS detection** from the nameserver lines of `/etc/resolv.conf`:
  - lines starting with `#` or `;` are skipped;
  - all nameservers `127.0.0.1` → Custom;
  - Cloudflare or Google only if **all** nameservers match;
  - all local/private → DHCP;
  - anything else → Custom;
  - no nameserver → DHCP.
- **Gateway ping** with packet loss, in a background thread. It uses an unprivileged ICMP datagram socket and falls back to `ping(8)`. `ping.conf` sets `net.ipv4.ping_group_range` so the socket is allowed.
- IP address and traffic detection (from the described physical link).
- **Known networks** come from **wpa_supplicant's control UNIX socket** (`LIST_NETWORKS`), not `wpa_cli`.
  - Socket directory: `$SYSTRAY_WPA_DIR`, default `/var/run/wpa_supplicant`; the socket is `<dir>/<iface>`.
  - If the socket is unusable, it falls back to `~/.config/systray/known_networks` (one SSID per line). **[Uncertain]** The assistant designed this fallback; it wasn't requested.
- **Encryption detection** from libnl scan data: Open, WEP, WPA, WPA2, WPA3.
  - OWE-only counts as open.
  - WPA2/WPA3 mixed mode is reported as WPA2.
  - The strongest AP represents an ESSID.
- **Per-row icons:** a closed padlock for WPA/WPA2/WPA3, an **open padlock for WEP**, an **exclamation mark for open** networks. Hovering the icon shows a tooltip with the encryption type.
- **Connecting:** known networks and open networks connect directly. **A password is asked for only for unknown WEP/WPA\* networks** ("WEP key" / "Network password").
- When the connection is wired (no wireless interface in use), the Wi-Fi lists are hidden. This was the existing behaviour and wasn't revisited.

### 2.5 Battery (battery.c) [Implemented]
- The icon shows the overall battery % next to it, in gray. The battery icon is skipped when there's no battery (`init` returns -1). **[Uncertain]** This was an assistant decision.
- Overall % = Σ now / Σ full across all batteries (energy- or charge-based sysfs), up to 8 batteries.
- The popup shows battery size and state ("Not charging" is shown as "Holding"), plus an optional charge-threshold subtitle (`charge_control_end_threshold`).
- One line per battery when there is more than one.
- Three power-profile buttons (Power-saver, Balanced, Performance) run `power-profile.sh`.
- The current profile is read from sysfs: `/sys/firmware/acpi/platform_profile`, otherwise cpu0's `scaling_governor` / `energy_performance_preference`. `powerprofilesctl` is not used.
- Tooltip with the overall %.

### 2.6 Helper scripts

**network-setup.sh** [Implemented]
- `WIFI <interface> <essid> <address> <open|wep|wpa|wpa2|wpa3>`. The secret comes only from the environment variable `WIFI_PASSWORD`, never an argument. `<address>` is the BSSID of the strongest AP; it is pinned only when `PIN_BSSID=1`.
- `REBIND <interface>`:
  - the interface name must match `^[A-Za-z0-9][A-Za-z0-9._-]*$`, so no leading `-` that dhcpcd would read as an option;
  - when not root it re-runs itself: `sudo -n -- <script> REBIND <iface>`;
  - as root it runs `dhcpcd -n -- <iface>`.
- `DNS <dhcp|cloudflare|google|custom> [interface]`:
  - **dhcp** renews the lease on the given interface (same path as REBIND), so dhcpcd rewrites `resolv.conf`. Without an interface nothing changes.
  - **cloudflare** 1.1.1.1/1.0.0.1; **google** 8.8.8.8/8.8.4.4; **custom** 127.0.0.1.
  - Writes `/etc/resolv.conf` (`RESOLV_CONF` overrides the path), through `sudo -n tee` when not root, and keeps search/domain/options lines.
- Uses `wpa_cli` for configuration (add/set/enable/select/save):
  - open → `key_mgmt NONE`;
  - WEP → `key_mgmt NONE`, `wep_tx_keyidx 0`, `auth_alg "OPEN SHARED"`, `wep_key0` (5/13 ASCII characters or 10/26 hex digits);
  - WPA → WPA-PSK, proto WPA; WPA2 → WPA-PSK, proto RSN; WPA3 → SAE with `ieee80211w 2`.

  WPA\* passwords must be 8–63 characters. A newly created secured network without a password is removed again. If the encryption argument is missing: WPA2 when a password is given, otherwise open.
- Extends PATH with the sbin directories and logs through `logger` (tag `network-setup`).

**power-profile.sh** [Implemented]
- `Power-saver|Balanced|Performance|get` (normalized to `power-saver|balanced|performance`).
- Applies to **every** CPU's cpufreq (governor, then EPP unless performance) and to `/sys/firmware/acpi/platform_profile` if present.
- Re-executes itself as `sudo -n -- <script> <profile>` when not root.

**display-change.sh** [Implemented; tested in simulation only]
- No arguments: **run by udev** (`89-kwort-video.rules`, DRM `change` uevent on `card0`) when a display is plugged in or unplugged.
  - It detaches (`setsid bash "$0" --detached`), sleeps 2 s, and runs under the lock `/run/kwort-display.lock` (flock fd 9).
  - It gets the user and the session's `DISPLAY`/`XAUTHORITY`/`XDG_RUNTIME_DIR`/`KWORT_PANEL` from `/proc/<pid>/environ` of kwort-systray, otherwise the panel.
  - Output names come from `xrandr --query`. Connected outputs are remembered in `/run/kwort-display.state`.
  - An unplugged but still active output gets `--off`, with a primary handoff (eDP/LVDS first).
  - A newly connected output gets a "jumpstart": `--off`, sleep 1, `--auto --right-of <primary>`.
  - After any change it runs `--restart-panel` as the desktop user.
- `--restart-panel` (also used by kwort-systray):
  - per-user lock `/run/user/<uid>/kwort-panel.lock` (fd 8, `flock -w 30`);
  - stops **all** panel and kwort-systray processes of the user (SIGTERM, up to 3 s, then SIGKILL), which cleans up duplicates;
  - starts one panel (unless something respawned it in the meantime), then the systray, each with its original command line and environment from `/proc`. kwort-systray's environment is unreadable because of setcap, so the panel's is used;
  - started processes get `8>&- 9>&-` so they don't inherit the locks.

  This fixed the duplicate tint2 copies the user reported.
- The panel is configurable: `KWORT_PANEL` (default `tint2`, overridable from the environment).

**compositor.sh** [Implemented; tested in simulation]
- POSIX sh. `KWORT_COMPOSITOR` (default `xcompmgr -c -f -C -r 5 -o .6 -D 5`, overridable from the environment).
- `start` (the default, does nothing if already running), `stop` (pkill, 2 s, then KILL), `restart`, `disable` (removes the symlink, then stops), `enable` (creates the symlink, then starts), `status` (e.g. `enabled running`).
- `/etc/X11/xinit/xinitrc.d/0-compositor.sh` is a symlink to this script. When **sourced** by xinitrc (`$0` is not `compositor.sh`), it only starts the compositor in the background and never exits.
- The symlink is changed through `sudo -n <script> --link on|off` (internal, root only).

### 2.7 System configuration files [Implemented]
- **89-kwort-video.rules:** gives the backlight `brightness` file to group `video` with g+w; runs `display-change.sh` on a DRM change event on `card0`.
- **kwort-systray.sudoers** → `/etc/sudoers.d/kwort-systray` (0440). `%users` (alias `KWORT_USERS`) may run, NOPASSWD, as root:
  - `power-profile.sh power-saver|balanced|performance`;
  - `compositor.sh --link on|off`;
  - `tee /etc/resolv.conf` (both `/usr/bin` and `/bin`);
  - `network-setup.sh REBIND *`.

  `requiretty` is off for these commands. dhcpcd is deliberately not allowed directly.
- **ping.conf** → `/etc/sysctl.d/ping.conf`: `net.ipv4.ping_group_range=0 2147483647`.

## 3. Business rules
- Secrets (the Wi-Fi password) are passed to the script **only** through the `WIFI_PASSWORD` environment variable, never as an argument.
- The password dialog appears only for **unknown** networks that use **WEP or WPA\***. Known and open networks never prompt.
- The ESSID is a plain argument and is not masked in logs.
- Network icon: blue normally, red when down. Wi-Fi glyph or Ethernet glyph depending on the link, yellow "V" badge when a VPN is active. Audio icon: yellow only when muted. Battery: gray with % text.
- Only real Ethernet counts as wired. VPN, PPP and TAP don't.
- The same power profile is applied to all CPUs.
- The user asked: **do not add new external commands** beyond those already in use; ask if something is needed.
- Brightness changes use sysfs only; power profile reading and writing use sysfs only.
- Every sudoers rule allows one exact command line (REBIND validates its argument itself). Never allow a tool whose options could run code as root (e.g. `dhcpcd -c`).
- You can't disable the last active display. Changing or disabling the primary restarts the panel.
- `make install` creates the compositor symlink only on the first install, so a user's *disable* survives reinstalls.

## 4. User workflows
1. **Connect to a known or open Wi-Fi network:** open the network popup → (optionally Scan) → click the network → the script is called with `WIFI`.
2. **Connect to an unknown secured network:** click the network → password dialog → the script is called with the arguments and the `WIFI_PASSWORD` environment variable.
3. **Change DNS:** click a provider button → the script runs `DNS <name>` (`DNS dhcp <iface>` for DHCP).
4. **Renew the lease:** click "Rebind IP".
5. **Check encryption:** hover the lock or exclamation icon in a network row.
6. **See the connection type:** the icon shows Wi-Fi or Ethernet and a "V" badge for a VPN; the tooltip shows details.
7. **Audio:** scroll over the icon for volume; middle-click to mute; the popup for devices.
8. **Display:** popup → drag monitors, left-click to set primary, right-click → Disable display, brightness slider or scroll.
9. **Compositor:** display popup → Compositor management → Restart / Stop / Disable / Enable.
10. **Plug or unplug a monitor:** handled automatically by udev and `display-change.sh`; the panel restarts.
11. **Battery:** popup → per-battery state, choose a power profile.
12. **Close a popup:** click outside.

## 5. Nonfunctional requirements
- Stack: C (gnu11), Xlib, Cairo (xlib surfaces, double-buffered), Pango.
- Builds must be warning-clean with `-Wall -Wextra`, including `-Wformat-truncation`: bounded `%.Ns` precisions (e.g. `%.15s` for interface names), and `_GNU_SOURCE` guarded with `#ifndef` in every `.c`.
- Runs as an unprivileged user. Privileged actions go through `sudo -n` (narrow sudoers file), file capabilities, sysctl or udev rules.
- Packaging support (`DESTDIR`): no changes to the running system during staged installs.
- Responsive UI: blocking work runs in threads (PulseAudio threaded mainloop, ping thread) or in double-forked helpers. `SIGCHLD` must **not** be set to `SIG_IGN` (it broke `pclose`).
- Debuggability: `app_run_ex` logs exec details, `load_known` logs socket failures, the tray logs re-docking, and scripts log to syslog (`network-setup`, `display-change`, `compositor`).
- Robust panel handling: never two panels or systrays after a restart; locks are never inherited by started processes.

## 6. Technical constraints
- Linux and X11 only. Libraries: x11, xrandr, cairo, cairo-xlib, pangocairo, pango, glib-2.0, libpulse, libnl-3.0, libnl-genl-3.0, pthread, m.
- The scan trigger needs `CAP_NET_ADMIN` → `setcap "cap_net_admin+ep"` on the binary.
- The ICMP ping socket needs `net.ipv4.ping_group_range` to include the user's group (`ping.conf`).
- The user's PATH normally lacks the sbin directories. PATH is extended in `network.c` (`extend_path`) and in the scripts (`/sbin`, `/usr/sbin`, …). `wpa_cli` is `/usr/sbin/wpa_cli` on the user's system.
- wpa_supplicant control socket access requires membership in the group set by `ctrl_interface ... GROUP=` (not verified on the user's system).
- **Makefile:**
  - `TARGET := kwort-systray`; `PREFIX ?= /usr`; `SCRIPTDIR ?= /usr/lib/kwort`; `UDEVDIR ?= /etc/udev/rules.d`; `SYSCTLDIR ?= /etc/sysctl.d`; `XINITRCDIR ?= /etc/X11/xinit/xinitrc.d`; `SUDOERSDIR ?= /etc/sudoers.d`; `SUDO ?= sudo` (empty when `DESTDIR` is set).
  - `make install` installs the binary (with `setcap`) and the four scripts (755). It removes the old `disable-display.sh`. It installs the udev rules (644) and `ping.conf` (644).
  - It creates the compositor symlink on the first install only.
  - It installs the sudoers file (0440) after rewriting its paths to `SCRIPTDIR` and checking it with `visudo -cf` when available.
  - Without `DESTDIR` it also runs `sysctl -p ping.conf`, `udevadm control --reload-rules` and `udevadm trigger --subsystem-match=backlight`.
- Environment variables:
  - kwort-systray: `SYSTRAY_NET_SCRIPT`, `SYSTRAY_POWER_SCRIPT`, `SYSTRAY_DISPLAY_SCRIPT`, `SYSTRAY_COMPOSITOR_SCRIPT`, `SYSTRAY_WPA_DIR`.
  - Scripts: `WIFI_PASSWORD`, `WPA_CTRL`, `PIN_BSSID`, `RESOLV_CONF`, `KWORT_PANEL`, `KWORT_COMPOSITOR`.
- Groups: `users` (sudoers), `video` (backlight), wpa_supplicant's control group (Wi-Fi).
- The assistant's environment has no C toolchain (§ status legend).

## 7. Requirements introduced during iterations (all [Implemented])
- Signal quality labels in the network widget; Scan button.
- Debug logging in `app_run`; blue network icon; `#ifndef _GNU_SOURCE`.
- Brightness through sysfs.
- ESSID as the third script argument → later `WIFI <interface> <essid> <address>`; known-network detection; password dialog; password as an environment variable.
- DNS "(?)" help inside the Custom button, centred; network icon raised about 2 px.
- Creation of `network-setup.sh`; scripts in `/usr/lib/kwort/`.
- Makefile: install rule with sudo, udev rules install, DESTDIR, PREFIX=/usr, TARGET=kwort-systray, setcap.
- "Rebind IP" button and the `REBIND` mode.
- sbin PATH handling.
- `power-profile.sh` and sysfs-based profile handling (all CPUs).
- Encryption detection, lock and exclamation icons, encryption tooltip, 5th script argument, WEP and open support in the script.
- Known networks through wpa_supplicant's UNIX socket (default `/var/run/wpa_supplicant`, override `SYSTRAY_WPA_DIR`).
- DNS detection fixes (commented lines, all-match rule, 127.0.0.1 = Custom); `DNS dhcp <iface>` renews the lease.
- Display: right-click "Disable display" with primary handoff; panel restart on primary change.
- `disable-display.sh` renamed to `display-change.sh`: udev hotplug handling, output jumpstart, shared `--restart-panel` with locking (fixed duplicate tint2).
- Compositor management menu and `compositor.sh`; xinitrc.d symlink. **[User-verified]** Icons come back after a compositor restart (tray re-dock fix).
- Sudoers file; REBIND re-runs the script through sudo, interface-name hardening.
- README with screenshots.
- `KWORT_PANEL` / `KWORT_COMPOSITOR` configurable and overridable from the environment.
- `ping.conf` sysctl file (added by the user) plus Makefile fixes (missing slash; `sysctl -p` only without `DESTDIR`).
- Ethernet icon when wired (real Ethernet only); VPN detection with Wi-Fi signal and a "V" badge. **Latest:** a `-Wformat-truncation` fix in `vpn_up_iface()`.

## 8. Requirements modified or removed (not current)
- `brightnessctl` for brightness → **removed**, replaced by direct sysfs writes.
- `powerprofilesctl` for power profiles → **removed**, replaced by sysfs.
- Known-network lookup through `wpa_cli list_networks` in `network.c` → **replaced** by the control-socket client. The script still uses `wpa_cli`; the user explicitly scoped the change to `network.c`.
- Script arguments originally `WIFI <ssid> <ssid>` with the password as the last argument → **superseded** by `WIFI <interface> <essid> <address> <encryption>` with the password only in the environment.
- Network icon gray → blue; network icon position lowered → raised 2 px.
- The old `Ap.secure` boolean → replaced by an `enc` type.
- All networks previously treated as needing a password → only WEP/WPA\*.
- `signal(SIGCHLD, SIG_IGN)` → removed (double-fork launcher).
- Masking the ESSID in logs → explicitly rejected by the user.
- `DNS dhcp` "changes nothing" → now renews the DHCP lease when an interface is given.
- `REBIND` through `sudo -n dhcpcd -n <iface>` → the script re-runs itself as root, validates the name, and runs `dhcpcd -n -- <iface>`.
- `disable-display.sh` → renamed `display-change.sh` (the Makefile removes the old file).
- "Any default route not on Wi-Fi is wired" → only real Ethernet is wired. VPNs are a badge over the physical link.
- Signal quality "Good" for both middle bands → the code uses **Fair** for −71..−80.

## 9. Known limitations
- The C code is not compiled by the assistant. Compositor, display-change and REBIND paths are untested on real hardware or real sudo, except where marked [User-verified].
- WPA2/WPA3-**Enterprise (802.1X/EAP)** networks are classified as WPA2 but can't be set up with a password-only dialog.
- WEP may be disabled in many `wpa_supplicant` builds; the script then fails with a message.
- WPA2/WPA3 transition networks are shown as WPA2.
- The password dialog is ASCII-only, with no paste.
- The known-network list from the socket doesn't decode wpa_supplicant's `\xNN` escapes for non-printable SSID characters.
- The control-socket client binds a temporary socket in `/tmp`, so it needs write access there and the socket group's permission.
- Hidden networks aren't supported (**[Uncertain]**, never discussed).
- WPA3 handling in the script relies on assumed wpa_supplicant options (`sae_password`), unverified.
- VPN detection is IPv4-only. Any VPN interface that is up (e.g. always-on `tailscale0`) shows the badge.
- Wi-Fi lists are hidden while the connection is wired.
- The sudoers file requires root ownership; a `DESTDIR` package must set it. `%users` may need changing (e.g. `%wheel`).
- `display-change.sh` only watches `card0`.

## 10. Unresolved questions
- The intended meaning of the `<address>` argument beyond the strongest AP's BSSID (currently used only when `PIN_BSSID=1`).
- Whether the omitted mockup features (Wi-Fi band toggle, header toggle) are wanted.
- Whether the `known_networks` file fallback should stay.
- Whether enterprise (EAP) networks should be supported.
- Whether the script should also move from `wpa_cli` to the socket (not requested).
- Which group wpa_supplicant's control socket uses on Kwort (the README uses `netdev` as an example).
- Whether always-on VPN interfaces should be ignored, or only VPNs that carry the default route counted.
- Whether Wi-Fi lists should stay visible while wired.
- Behaviour on systems without a battery beyond skipping the icon is unconfirmed.

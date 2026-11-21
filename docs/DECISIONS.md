# kwort-systray — Decision Log

> Only decisions supported by the development conversation are recorded. "Alternatives" lists only options that were actually mentioned or implied by a change of approach. Where the user didn't state the reason, it is marked **(rationale inferred)**. The assistant has never compiled the C code; the user builds it on Linux. Scripts were tested in a sandbox with fake commands.

## D1. Single C application with a `Module` interface
- **Decision:** one binary with four modules (audio, display, network, battery) plus `tray.c` as the host, sharing a UI toolkit through `common.h`.
- **Reason:** specified by the user's original request.
- **Alternatives:** none discussed.

## D2. Self-drawn popups with pointer grab
- **Decision:** borderless override-redirect popups, `XGrabPointer(owner_events=True)`, closed by an outside click; Cairo push_group double buffering.
- **Reason:** user requirement (borderless, closes on an outside click). Double buffering avoids flicker **(rationale inferred)**.
- **Alternatives:** none discussed.

## D3. Sysfs instead of helper commands
- **Brightness:** the user asked to write `/sys/class/backlight/<dev>/brightness` instead of using `brightnessctl`. Tradeoff: needs write permission, so a udev rules file ships (`89-kwort-video.rules`: group `video`, g+w).
- **Power profile:** the user asked for sysfs-based setting and reading instead of `powerprofilesctl`, applied to all CPUs per the user's rule. Tradeoff: needs root, so `power-profile.sh` re-runs itself through `sudo -n`.
- **Rejected:** `brightnessctl`, `powerprofilesctl`.

## D4. Privileged work delegated to scripts
- **Decision:** DNS, Wi-Fi and rebind in `network-setup.sh`; power profiles in `power-profile.sh`; later `compositor.sh` and `display-change.sh`. All are installed to `/usr/lib/kwort/`, and their paths can be overridden with `SYSTRAY_NET_SCRIPT`, `SYSTRAY_POWER_SCRIPT`, `SYSTRAY_COMPOSITOR_SCRIPT` and `SYSTRAY_DISPLAY_SCRIPT`.
- **Reason:** the user's request; keeps the tray unprivileged.
- **Tradeoff:** relies on `sudo -n`, which is now covered by D20.

## D5. Wi-Fi secrets only through the environment
- **Decision:** the password goes only in `WIFI_PASSWORD`, never as an argument.
- **Reason:** user correction after the assistant passed it as the last argument.
- **Related:** the last logged argument is the ESSID. The user rejected masking the ESSID in logs; only the environment value is masked.

## D6. Script argument contract
- **Decision:** `script WIFI <interface> <essid> <address> <encryption>`; `REBIND <interface>`; `DNS <name> [interface]`.
- **Reason:** specified by the user step by step (ESSID as the 3rd argument; address after the ESSID; encryption after the address; the user asked to add arguments after the ESSID rather than new commands). The optional interface for `DNS dhcp` was added so DHCP can renew the lease (D18).
- **Open point:** the meaning of `<address>` (U1).

## D7. Double fork launcher; no `SIG_IGN` on SIGCHLD
- **Decision:** `app_run_ex` double-forks.
- **Reason:** `signal(SIGCHLD, SIG_IGN)` made `pclose` return -1 (diagnosed after the user added debug output).
- **Rejected:** ignoring SIGCHLD.

## D8. PATH extension for sbin
- **Decision:** `extend_path()` in `network.c` and a PATH loop in the scripts add `/sbin` and `/usr/sbin`.
- **Reason:** the user's unprivileged PATH didn't contain `wpa_cli`/`dhcpcd` (user request).

## D9. Known networks from wpa_supplicant's UNIX socket (replaces `wpa_cli`)
- **Decision:** `network.c` talks directly to the control socket (`LIST_NETWORKS`). Directory `$SYSTRAY_WPA_DIR`, default `/var/run/wpa_supplicant`.
- **Reason:** user request, after earlier work on `wpa_cli`, which had failed with "command not found" and `pclose` issues.
- **Alternatives:** `wpa_cli` through `popen` (the previous implementation, **rejected/replaced**); a `~/.config/systray/known_networks` file (kept only as a fallback, the assistant's own addition, still present).
- **Tradeoffs:** no external binary or PATH dependency; the client needs socket access and writes a temporary socket under `/tmp`.
- **Scope:** `network-setup.sh` still uses `wpa_cli` (the user scoped the change to `network.c`).

## D10. Encryption detection through libnl IEs
- **Decision:** classify Open/WEP/WPA/WPA2/WPA3 from RSN/WPA IEs and the privacy bit. Ask for a password only for WEP/WPA\*. Icons: closed lock (WPA\*), open lock (WEP), exclamation mark (open). A hover tooltip shows the type.
- **Reason:** user request.
- **Sub-decisions the assistant made without a specific request:**
  - OWE-only counts as open.
  - SAE-only → WPA3; WPA2/WPA3 mixed mode → WPA2.
  - 802.1X/EAP → WPA2 (not supported separately).
  - The strongest BSS represents an ESSID.
- **Tradeoff:** enterprise networks look connectable but aren't.

## D11. The script supports WEP and open through `wpa_cli`
- **Decision:** open → `key_mgmt NONE`; WEP → `key_mgmt NONE` plus `wep_key0`; WPA3 → SAE. The user said `wpa_cli` supports WEP and open ("AFAIK") and asked for no new commands.
- **Tradeoff:** many wpa_supplicant builds disable WEP (known limitation).

## D12. Scan privilege through a file capability
- **Decision:** `setcap "cap_net_admin+ep"` in `make install` (user request); the scan trigger needs `CAP_NET_ADMIN`.
- **Consequence:** the process becomes non-dumpable, so its `/proc/<pid>/environ` can't be read by the user. `display-change.sh --restart-panel` therefore restarts the systray with the panel's environment.
- **Alternative:** running as root (not discussed).

## D13. Packaging-friendly Makefile
- **Decision:**
  - `DESTDIR` support, `PREFIX ?= /usr`, `TARGET := kwort-systray`;
  - install directories `SCRIPTDIR`, `UDEVDIR`, `SYSCTLDIR`, `XINITRCDIR`, `SUDOERSDIR`;
  - `SUDO` is empty with `DESTDIR`, and a `setcap` failure is tolerated with `DESTDIR`;
  - steps that change the running system (`sysctl -p`, the udev reload and trigger) run only without `DESTDIR`.
- **Reason:** user requests. When the user added the `sysctl -p` line, the assistant moved it into the no-DESTDIR block and fixed a missing slash, at the user's request.

## D14. `_GNU_SOURCE` through `#ifndef` in the sources
- **Decision:** a guard in each `.c`, removed from the Makefile.
- **Reason:** user request, to remove redefinition warnings.

## D15. Warning clean-up choices
- **Decision:** bounded `%.Ns` precisions and larger buffers for `-Wformat-truncation`. An invalid `direct_on_input` check in audio.c was removed. In `vpn_up_iface()`, `/sys/class/net` names of `IFNAMSIZ` or longer are skipped and copied with `%.15s`.
- **Reason:** compile output the user reported.

## D16. UI decisions (user-directed)
- Network icon blue, raised 2 px; the "(?)" help inside the Custom DNS button, centred; signal-quality labels (the code uses Excellent/Good/Fair/Unusable); Scan and Rebind IP buttons.
- Red network icon when down; yellow audio icon when muted; battery % beside the icon.

## D17. Decisions the assistant made without confirmation
- The battery icon is skipped when there's no battery.
- The mockup's Wi-Fi band toggle and header toggle were left out.
- Custom DNS → 127.0.0.1 (from the user's description of Custom).

## D18. DNS detection and DHCP renew
- **Decision:**
  - nameserver `127.0.0.1` means Custom;
  - Cloudflare or Google only when **all** nameservers match;
  - commented lines (`#`/`;`) are ignored;
  - `DNS dhcp <iface>` renews the lease through the same path as REBIND, instead of "changing nothing".
- **Reason:** user-reported misdetection; DHCP should actually restore the DHCP-provided servers **(rationale inferred from the change)**.

## D19. Display management: disable display, panel restart, hotplug script
- **Decision:**
  - A right-click "Disable display" menu. It refuses to disable the last active display, and when the primary is disabled, moves the primary to the internal panel first. One xrandr call, so there is never a moment without a primary.
  - Changing or disabling the primary restarts tint2 and the systray, because tint2 stays on the output that was primary when it started.
  - `disable-display.sh` was renamed to `display-change.sh`, which handles udev hotplug (off for unplugged outputs, a "jumpstart" for new ones).
  - A single shared `--restart-panel` routine with a per-user lock. It kills all copies and starts exactly one panel and one systray, with their original command line and environment.
  - Output names come from `xrandr --query`, not `/sys/class/drm`, because the sysfs and xrandr names differ.
- **Reason:** user requests. The user reported two tint2 copies after restarts, which led to the shared locked routine and the "don't start if respawned" check. An inherited lock fd caused a hang, hence `8>&- 9>&-`.
- **Alternatives:** separate restart logic in C and in the script (replaced by the single script routine).

## D20. Compositor management and the xinitrc.d symlink
- **Decision:** a display-popup menu (Restart / Stop / Disable / Enable) backed by `compositor.sh`. Enabling or disabling creates or removes the symlink `/etc/X11/xinit/xinitrc.d/0-compositor.sh` → `compositor.sh`. When sourced by xinitrc, the script only starts the compositor and never exits. `make install` creates the symlink only on the first install, so a user's *disable* survives.
- **Reason:** user request, as a workaround for UI glitches or a misbehaving compositor.

## D21. Re-dock on tray restart
- **Decision:** `check_tray(force)`. A `MANAGER` ClientMessage forces a re-dock, and `ReparentNotify` to the root window sets `redock_pending`.
- **Reason:** after a compositor restart, tint2 re-executes itself with the same tray owner window ID, so comparing owners missed it and the icons disappeared. **User-verified** ("It worked, icons come back now").

## D22. Narrow sudoers file
- **Decision:** ship `kwort-systray.sudoers` with one exact command line per rule for `%users`. `requiretty` is off. The file is validated with `visudo -cf` at install and installed 0440. REBIND became "the script re-runs itself as root" (`network-setup.sh REBIND *`) instead of allowing `dhcpcd -n *`. Interface names can't start with `-`.
- **Reason:** user request (scripts must not fail on `sudo -n`). The REBIND redesign closes a root hole: an interface name like `-cfoo` would have made dhcpcd run `./foo` as root.
- **Alternatives:** allowing `dhcpcd -n *` directly (rejected for the reason above).
- **Open:** the group (`%users` vs. `%wheel`) is the user's choice; the file must be owned by root.

## D23. Configurable panel and compositor
- **Decision:** `KWORT_PANEL` (default `tint2`) in `display-change.sh` and `KWORT_COMPOSITOR` (default `xcompmgr …`) in `compositor.sh`, both `${VAR:-default}`, so the environment overrides them and an empty value falls back. When run from udev, `KWORT_PANEL` is read from kwort-systray's environment and passed explicitly through `su`.
- **Reason:** user request.

## D24. Unprivileged ICMP via sysctl
- **Decision:** `ping.conf` (`net.ipv4.ping_group_range=0 2147483647`) is installed to `/etc/sysctl.d`, rather than giving the binary `CAP_NET_RAW`.
- **Reason:** added by the user. It keeps the capability set at `cap_net_admin` only **(rationale inferred)**.

## D25. Wired icon and VPN badge
- **Decision:**
  - The described link is the lowest-metric non-VPN default route.
  - It shows an Ethernet glyph only for **real Ethernet** (`type` 1, no `tun_flags`, not wireless). VPN, PPP and TAP don't count.
  - A VPN (tun/WireGuard/TAP; with a default route, OpenVPN `def1`, or simply up) keeps the Wi-Fi signal of the link underneath and adds a yellow "V" badge, "· VPN" to the header and "— VPN `<iface>`" to the tooltip.
- **Reason:** user requests, in three steps: wired icon → only real Ethernet → show Wi-Fi under a VPN with a "V" indicator.
- **Tradeoffs:** IP, traffic and ping describe the physical link while a VPN is up. Always-up VPN interfaces (e.g. Tailscale) always show the badge. IPv4 routes only.

## Unresolved decisions / open questions
- **U1.** The intended use of `<address>`: currently the BSSID of the strongest AP, pinned only when `PIN_BSSID=1`.
- **U2.** Whether `network-setup.sh` should also move off `wpa_cli` to the control socket.
- **U3.** The wpa_supplicant control-socket group on Kwort (the sudoers group defaults to `%users`).
- **U4.** Whether to support enterprise (EAP) networks, hidden networks, or decode `\xNN` SSID escapes.
- **U5.** Whether to keep the `known_networks` file fallback.
- **U6.** Whether to reintroduce the omitted mockup features.
- **U7.** Whether always-up VPN interfaces should be ignored, and whether Wi-Fi lists should stay visible while wired.

## Verified against the current source
- No `popen("wpa_cli …")` remains in `network.c`; the only `popen` is the `ping` fallback.
- `SIGCHLD` is not set to ignore anywhere.
- `89-kwort-video.rules` contents are known (backlight group `video`; DRM change → `display-change.sh`).

## Still to verify
- Everything after the last user build compiles (the `vpn_up_iface()` warning fix).

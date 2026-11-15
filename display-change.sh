#!/bin/bash
# display-change.sh - display hot-plug handling and panel restart.
#
#   display-change.sh
#       Run by eudev (89-kwort-video.rules) on a DRM "change" uevent, i.e.
#       when a display is plugged in or unplugged:
#
#       unplugged display that is still on
#           -> xrandr --output X --off.  If it was the primary, the primary
#              moves to the laptop panel (eDP / LVDS) when connected,
#              otherwise to another active display.
#       newly plugged display
#           -> jumpstart the video link (some monitors show "no signal"
#              although X considers them active):
#                  xrandr --output X --off; sleep 1
#                  xrandr --output X --auto --right-of <primary>
#       either of the above
#           -> display-change.sh --restart-panel, as the desktop user.
#
#   display-change.sh --restart-panel
#       Restart the panel ($KWORT_PANEL, tint2 by default) and kwort-systray
#       so they follow the new layout (tint2 stays on the output that was
#       primary when it started).  Also used by kwort-systray itself after
#       changing or disabling the primary display.  Must run as the desktop
#       user.  Does nothing if the panel is not running.
#
# Output names and their state come from "xrandr --query", not from
# /sys/class/drm: sysfs names (HDMI-A-1) do not always match xrandr's
# (HDMI-1 under modesetting, HDMI-A-0 under amdgpu).
#
# The connected outputs are remembered in $STATE, so repeated uevents for
# the same change (and any uevents our own xrandr calls cause) do nothing.

# The panel (dock) that hosts the system tray.  Change it here, or export
# KWORT_PANEL in the desktop session (e.g. from ~/.bash_profile before
# startx), to use another panel.  The first word is the program name used to
# find the running panel; the whole line is only used to start it when the
# running panel's command line cannot be read (normally it is restarted with
# its own command line, so e.g. "tint2 -c <config>" is kept).
# Run from udev, the session's KWORT_PANEL is read from the running
# kwort-systray process.
KWORT_PANEL=${KWORT_PANEL:-tint2}
PANEL_NAME=${KWORT_PANEL%% *}             # process name, e.g. tint2
PANEL_NAME=${PANEL_NAME##*/}

log() { logger -t display-change "$*"; }

# ===========================================================================
# --restart-panel
#
# Everything that restarts the panel goes through here, under one per-user
# lock, so two restarts (udev + systray) can never overlap.  It stops *all*
# panel / kwort-systray processes of the user (cleaning up duplicates),
# waits until they are really gone, and starts one of each again, unless one
# is already running by then (panel respawned by the session / window
# manager), so there is never a second copy.
# ===========================================================================
restart_panel() {
    local uid dir
    uid=$(id -u)
    [ "$uid" -ne 0 ] || { log "--restart-panel must run as the desktop user"; return 1; }
    dir=/run/user/$uid
    [ -d "$dir" ] && [ -w "$dir" ] || dir=${TMPDIR:-/tmp}
    exec 8>>"$dir/kwort-panel.lock" || return 1
    flock -w 30 8 || { log "panel restart: lock busy for 30s, skipped"; return 1; }

    local -a t_pids s_pids t_env t_cmd s_env s_cmd
    mapfile -t t_pids < <(pgrep -x -u "$uid" "$PANEL_NAME")
    [ ${#t_pids[@]} -gt 0 ] || { log "$PANEL_NAME is not running: nothing to restart"; return 0; }
    mapfile -t s_pids < <(pgrep -x -u "$uid" kwort-systray)
    [ ${#t_pids[@]} -gt 1 ] && log "found ${#t_pids[@]} $PANEL_NAME processes (${t_pids[*]}); restarting only one"
    [ ${#s_pids[@]} -gt 1 ] && log "found ${#s_pids[@]} kwort-systray processes (${s_pids[*]}); restarting only one"

    # command line + environment of the oldest of each, so options such as
    # "tint2 -c <config>" are kept
    mapfile -d '' t_cmd 2>/dev/null < "/proc/${t_pids[0]}/cmdline"
    mapfile -d '' t_env 2>/dev/null < "/proc/${t_pids[0]}/environ"
    [ ${#t_cmd[@]} -gt 0 ] || read -r -a t_cmd <<<"$KWORT_PANEL"
    [ ${#t_env[@]} -gt 0 ] || mapfile -d '' t_env < <(env -0)
    if [ ${#s_pids[@]} -gt 0 ]; then
        mapfile -d '' s_cmd 2>/dev/null < "/proc/${s_pids[0]}/cmdline"
        # environ is unreadable when kwort-systray has file capabilities
        # (setcap makes it non-dumpable): use the panel's session environment
        mapfile -d '' s_env 2>/dev/null < "/proc/${s_pids[0]}/environ"
        [ ${#s_cmd[@]} -gt 0 ] || s_cmd=(kwort-systray)
        [ ${#s_env[@]} -gt 0 ] || s_env=("${t_env[@]}")
    fi

    # stop: systray first so it does not re-dock into a dying panel
    local -a all=("${s_pids[@]}" "${t_pids[@]}")
    local alive p _
    kill "${all[@]}" 2>/dev/null
    for _ in {1..15}; do                  # up to 3 s
        alive=0
        for p in "${all[@]}"; do kill -0 "$p" 2>/dev/null && alive=1; done
        [ "$alive" = 0 ] && break
        sleep 0.2
    done
    if [ "$alive" = 1 ]; then
        log "panel processes did not exit on SIGTERM; sending SIGKILL"
        kill -KILL "${all[@]}" 2>/dev/null
        sleep 0.3
    fi

    # start one of each, unless something already brought it back
    sleep 0.3
    if pgrep -x -u "$uid" "$PANEL_NAME" >/dev/null; then
        log "$PANEL_NAME was already started again by someone else (session or window manager); not starting another"
    else
        log "restarting: ${t_cmd[*]}"
        setsid env -i "${t_env[@]}" "${t_cmd[@]}" </dev/null >/dev/null 2>&1 8>&- 9>&- &
    fi
    sleep 1                               # let the panel claim the tray
    if [ ${#s_pids[@]} -gt 0 ]; then
        if pgrep -x -u "$uid" kwort-systray >/dev/null; then
            log "kwort-systray was already started again; not starting another"
        else
            log "restarting: ${s_cmd[*]}"
            # stderr is kept: when the systray asked for the restart this is
            # where its log was going
            setsid env -i "${s_env[@]}" "${s_cmd[@]}" </dev/null >/dev/null 8>&- 9>&- &
        fi
    fi
    return 0
}

if [ "$1" = --restart-panel ]; then
    restart_panel
    exit
fi

# ===========================================================================
# uevent handling
# ===========================================================================
if [ "$(id -u)" -eq 0 ]; then
    RUNDIR=/run
else
    RUNDIR=${XDG_RUNTIME_DIR:-/tmp}       # run by hand for testing
fi
STATE=$RUNDIR/kwort-display.state
LOCK=$RUNDIR/kwort-display.lock

# udev expects RUN programs to return quickly and may kill them: detach, and
# give the link a moment to settle (also merges bursts of uevents).
if [ "$1" != --detached ]; then
    setsid bash "$0" --detached "$@" </dev/null >/dev/null 2>&1 &
    exit 0
fi
shift
sleep 2

exec 9>"$LOCK"
flock 9                                   # one run at a time

# ---------------------------------------------------------------------------
# Desktop session: user, DISPLAY, XAUTHORITY and KWORT_PANEL.  Taken from a
# running X client of the user (kwort-systray first: it knows the session's
# KWORT_PANEL, and root can read its environ despite setcap; then the
# panel), falling back to the first logged-in user with :0 and
# ~/.Xauthority.
# ---------------------------------------------------------------------------
REAL_USER=$(id -un)
if [ "$(id -u)" -eq 0 ]; then
    pid=$(pgrep -o -x kwort-systray || pgrep -o -x "$PANEL_NAME")
    if [ -n "$pid" ]; then
        REAL_USER=$(ps -o user= -p "$pid" | tr -d ' ')
        while IFS= read -r -d '' kv; do
            case $kv in
                DISPLAY=*|XAUTHORITY=*|XDG_RUNTIME_DIR=*|KWORT_PANEL=?*) export "${kv?}" ;;
            esac
        done < "/proc/$pid/environ"
    else
        REAL_USER=$(who | awk '{print $1; exit}')
    fi
    [ -n "$REAL_USER" ] || { log "no desktop user found"; exit 0; }
    home=$(getent passwd "$REAL_USER" | cut -d: -f6)
    export DISPLAY=${DISPLAY:-:0}
    export XAUTHORITY=${XAUTHORITY:-$home/.Xauthority}
    export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u "$REAL_USER")}
fi
[ -n "$DISPLAY" ] || { log "no X display"; exit 0; }
command -v xrandr >/dev/null || { log "xrandr not found"; exit 0; }
USER_ID=$(id -u "$REAL_USER")

# Run a command as the desktop user (directly when we already are that user).
# 9>&-: our lock is never handed to anything we start.
as_user() {
    if [ "$(id -u)" -eq 0 ] && [ "$USER_ID" -ne 0 ]; then
        su -s /bin/bash "$REAL_USER" -c "$(printf '%q ' "$@")" 9>&-
    else
        "$@" 9>&-
    fi
}

xr() { as_user xrandr "$@"; }

# ---------------------------------------------------------------------------
# Current outputs:  name  connected|disconnected  primary(0/1)  active(0/1)
# ---------------------------------------------------------------------------
query() {
    xr --query | awk '
        $2 == "connected" || $2 == "disconnected" {
            prim = 0; act = 0
            for (i = 3; i <= NF; i++) {
                if ($i == "primary") prim = 1
                if ($i ~ /^[0-9]+x[0-9]+[+-][0-9]+[+-][0-9]+$/) act = 1
            }
            print $1, $2, prim, act
        }'
}

OUT=$(query)
[ -n "$OUT" ] || { log "xrandr --query failed (DISPLAY=$DISPLAY)"; exit 0; }

connected_now=$(awk '$2 == "connected" {print $1}' <<<"$OUT")
if [ -f "$STATE" ]; then
    connected_before=$(cat "$STATE")
else
    # first event since boot: what is already on was set up at login
    connected_before=$(awk '$2 == "connected" && $4 {print $1}' <<<"$OUT")
fi

field() { awk -v n="$1" -v f="$2" '$1 == n {print $f}' <<<"$OUT"; }

changed=0

# ---- unplugged displays that are still on --------------------------------
for name in $(awk '$2 == "disconnected" && $4 {print $1}' <<<"$OUT"); do
    args=(--output "$name" --off)
    if [ "$(field "$name" 3)" = 1 ]; then
        # it was the primary: pick a new one.  Prefer an active laptop panel,
        # then any active display; only switch something on if nothing else
        # would be left on.
        np=$(awk -v d="$name" '$1 != d && $2 == "connected" && $4 && $1 ~ /^(eDP|LVDS)/ {print $1; exit}' <<<"$OUT")
        [ -n "$np" ] || np=$(awk -v d="$name" '$1 != d && $2 == "connected" && $4 {print $1; exit}' <<<"$OUT")
        if [ -n "$np" ]; then
            args+=(--output "$np" --primary)
        else
            np=$(awk '$2 == "connected" && $1 ~ /^(eDP|LVDS)/ {print $1; exit}' <<<"$OUT")
            [ -n "$np" ] || np=$(awk '$2 == "connected" {print $1; exit}' <<<"$OUT")
            [ -n "$np" ] && args+=(--output "$np" --auto --primary)
        fi
    fi
    log "display $name was disconnected: xrandr ${args[*]}"
    xr "${args[@]}" || log "xrandr ${args[*]} failed"
    changed=1
done

# ---- newly plugged displays: jumpstart the video link --------------------
for name in $connected_now; do
    grep -qxF "$name" <<<"$connected_before" && continue
    ref=$(awk -v d="$name" '$1 != d && $2 == "connected" && $4 && $3 {print $1; exit}' <<<"$OUT")
    [ -n "$ref" ] || ref=$(awk -v d="$name" '$1 != d && $2 == "connected" && $4 {print $1; exit}' <<<"$OUT")
    log "display $name was connected: resetting its video link"
    xr --output "$name" --off
    sleep 1
    if [ -n "$ref" ]; then
        xr --output "$name" --auto --right-of "$ref"
    else
        xr --output "$name" --auto
    fi || log "xrandr --output $name --auto failed"
    changed=1
done

printf '%s\n' $connected_now > "$STATE"
[ "$changed" = 1 ] || exit 0

# KWORT_PANEL passed explicitly: su does not guarantee to keep the environment
as_user env KWORT_PANEL="$KWORT_PANEL" bash "$(readlink -f "$0")" --restart-panel
exit 0

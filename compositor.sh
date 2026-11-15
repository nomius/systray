#!/bin/sh
# compositor.sh - start / stop / restart the X compositor, and enable or
# disable starting it with X.  Used by the systray display widget
# ("Compositor management").
#
#   compositor.sh [start]   start the compositor (if not already running)
#   compositor.sh stop      stop it
#   compositor.sh restart   stop it and start it again
#   compositor.sh disable   stop it and remove $LINK  (not started with X)
#   compositor.sh enable    create $LINK and start it (started with X again)
#   compositor.sh status    print "enabled|disabled running|stopped"
#
# $LINK (/etc/X11/xinit/xinitrc.d/0-compositor.sh) is a symlink to this
# script.  xinitrc SOURCES it ([ -x "$f" ] && . "$f"), so when sourced this
# file only starts the compositor in the background: it never exits (that
# would end xinitrc), never looks at xinitrc's arguments and never blocks.
#
# enable / disable write to /etc: run as a normal user they re-run this
# script through "sudo -n" for that step (allow it in sudoers with NOPASSWD,
# like power-profile.sh).

# The compositor command line.  Change it here, or export KWORT_COMPOSITOR in
# the desktop session (e.g. from ~/.bash_profile before startx, so both
# xinitrc and the systray see it), to use another compositor.
KWORT_COMPOSITOR=${KWORT_COMPOSITOR:-"xcompmgr -c -f -C -r 5 -o .6 -D 5"}

case ${0##*/} in
*compositor.sh) kwort_comp_sourced=0 ;;   # run as a command
*)              kwort_comp_sourced=1 ;;   # sourced by xinitrc ($0 = xinitrc)
esac

if [ "$kwort_comp_sourced" = 1 ]; then
    # ---- sourced by xinitrc: just start the compositor -------------------
    if command -v "${KWORT_COMPOSITOR%% *}" >/dev/null 2>&1; then
        $KWORT_COMPOSITOR </dev/null >/dev/null 2>&1 &
    fi
    unset kwort_comp_sourced
else
    # ---- run as a command ------------------------------------------------
    LINK=/etc/X11/xinit/xinitrc.d/0-compositor.sh
    SELF=$(readlink -f "$0")
    PROG=${0##*/}
    NAME=${KWORT_COMPOSITOR%% *}           # process name, e.g. xcompmgr
    NAME=${NAME##*/}
    UID_=$(id -u)

    log() {
        printf '%s: %s\n' "$PROG" "$*" >&2
        command -v logger >/dev/null 2>&1 && logger -t compositor "$*"
    }

    running() { pgrep -x -u "$UID_" "$NAME" >/dev/null 2>&1; }

    start() {
        if running; then
            log "$NAME is already running"
            return 0
        fi
        command -v "${KWORT_COMPOSITOR%% *}" >/dev/null 2>&1 ||
            { log "ERROR: '${KWORT_COMPOSITOR%% *}' not found"; return 1; }
        [ -n "$DISPLAY" ] || { log "ERROR: DISPLAY is not set"; return 1; }
        log "starting: $KWORT_COMPOSITOR"
        # detached, so it outlives whatever called us (e.g. the systray)
        setsid $KWORT_COMPOSITOR </dev/null >/dev/null 2>&1 &
        return 0
    }

    stop() {
        running || { log "$NAME is not running"; return 0; }
        log "stopping $NAME"
        pkill -x -u "$UID_" "$NAME"
        i=0
        while running && [ $i -lt 10 ]; do sleep 0.2; i=$((i + 1)); done
        if running; then
            log "$NAME did not exit on SIGTERM; sending SIGKILL"
            pkill -KILL -x -u "$UID_" "$NAME"
            sleep 0.2
        fi
        return 0
    }

    # create / remove the xinitrc.d symlink; needs root
    set_link() {   # set_link on|off
        if [ "$UID_" -ne 0 ]; then
            sudo -n "$SELF" --link "$1" ||
                { log "ERROR: 'sudo -n $SELF --link $1' failed (allow it in sudoers with NOPASSWD)"; return 1; }
            return 0
        fi
        if [ "$1" = on ]; then
            mkdir -p "${LINK%/*}" && ln -sfn "$SELF" "$LINK" ||
                { log "ERROR: cannot create $LINK"; return 1; }
            log "enabled: $LINK -> $SELF"
        else
            rm -f "$LINK" || { log "ERROR: cannot remove $LINK"; return 1; }
            log "disabled: removed $LINK"
        fi
    }

    case ${1:-start} in
        start)   start ;;
        stop)    stop ;;
        restart) stop && start ;;
        disable) set_link off && stop ;;
        enable)  set_link on && start ;;
        --link)  # internal: the root part of enable / disable
                 [ "$UID_" -eq 0 ] || { log "ERROR: --link needs root"; exit 1; }
                 case $2 in on|off) set_link "$2" ;; *) exit 2 ;; esac ;;
        status)
            if [ -L "$LINK" ] || [ -e "$LINK" ]; then s=enabled; else s=disabled; fi
            if running; then r=running; else r=stopped; fi
            echo "$s $r" ;;
        *)
            echo "usage: $PROG [start|stop|restart|enable|disable|status]" >&2
            exit 2 ;;
    esac
fi

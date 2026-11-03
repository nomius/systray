#!/bin/bash


PROG=${0##*/}
log() { logger -t disable-display "$PROG $*"; }
die() { log "ERROR: $*"; exit 1; }


USER_ID=$(id -u)
if [ "$USER_ID" -eq 0 ]; then
    # Running as root (via udev), find the logged-in desktop user
    REAL_USER=$(who | awk '{print $1}' | head -n1)
    USER_ID=$(id -u "$REAL_USER")
    export DISPLAY=:0
    export XAUTHORITY="/home/$REAL_USER/.Xauthority"
    export XDG_RUNTIME_DIR="/run/user/$USER_ID"
fi

# 2. Path to DRM sysfs graphics class
DRM_DIR="/sys/class/drm"

# 3. Dynamic Scan: Loop through every port on every graphics card
for port_dir in "$DRM_DIR"/card*-*; do
    # Skip if the directory doesn't exist (failsafe)
    [ -d "$port_dir" ] || continue
    
    # Extract port name (e.g., HDMI-A-1, DP-1)
    PORT_NAME=$(basename "$port_dir" | cut -d'-' -f2-)
    STATUS=$(cat "$port_dir/status")

    if [ "$STATUS" = "disconnected" ]; then
        # Dynamically act based on whether X11 or Wayland is running
        if [ -n "$DISPLAY" ] && command -v xrandr &> /dev/null; then
            # X11 fallback: Turn off the disconnected port safely
            xrandr --output "$PORT_NAME" --off 2>/dev/null
			su "$REAL_USER" -c "pkill tint2" 2>/dev/null
			sleep 0.5
			su "$REAL_USER" -c "DISPLAY=$DISPLAY XAUTHORITY=$XAUTHORITY tint2 &" 2>/dev/null

        elif [ -n "$WAYLAND_DISPLAY" ]; then
            # Wayland actions usually happen via compositor-specific IPC (wlr-randr, hyprctl, etc.)
            # Insert compositor-specific command here if needed
            :
        fi
        
        log "Port ${PORT_NAME} was disconnected"
    fi
done


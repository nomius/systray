#!/usr/bin/env bash
#
# power-profile.sh - power profile switching through sysfs, used by the
# systray battery widget.
#
#   power-profile.sh <Power-saver|Balanced|Performance>   apply a profile
#   power-profile.sh get                                  print current profile
#
# The profile is applied to EVERY CPU that has a cpufreq directory:
#   /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor
#   /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference
# and, when the firmware exposes it, to the platform profile:
#   /sys/firmware/acpi/platform_profile
#
#   profile       governor                     EPP                    platform
#   power-saver   powersave                    power / balance_power  low-power
#   balanced      powersave (or schedutil)     balance_performance    balanced
#   performance   performance                  performance            performance
#
# Writing to sysfs needs root: when run as a normal user the script re-runs
# itself through "sudo -n" (allow it in sudoers with NOPASSWD).

set -u -o pipefail

PROG=${0##*/}
CPU=/sys/devices/system/cpu
PLATFORM=/sys/firmware/acpi/platform_profile

log() { printf '%s: %s\n' "$PROG" "$*" >&2; }
die() { log "ERROR: $*"; exit 1; }

usage() {
    echo "usage: $PROG <Power-saver|Balanced|Performance|get>" >&2
    exit 2
}

# power-saver | balanced | performance
normalize() {
    local p=${1,,}
    p=${p//_/-}
    p=${p// /-}
    case $p in
        power-saver|powersaver|power-save|powersave|saver|low-power) echo power-saver ;;
        balanced|balance)                                           echo balanced ;;
        performance|perf)                                           echo performance ;;
        *) return 1 ;;
    esac
}

# first of the candidates that appears in the space separated list $1
pick() {
    local avail=" $1 " c
    shift
    for c in "$@"; do
        if [[ $avail == *" $c "* ]]; then echo "$c"; return 0; fi
    done
    return 1
}

read_file() { [[ -r $1 ]] && tr -d '\n' <"$1"; }

# ---------------------------------------------------------------------------
current_profile() {
    local v gov epp
    if v=$(read_file "$PLATFORM") && [[ -n $v ]]; then
        case $v in
            low-power|quiet|cool) echo power-saver; return ;;
            balanced)             echo balanced;    return ;;
            performance)          echo performance; return ;;
        esac
    fi

    gov=$(read_file "$CPU/cpu0/cpufreq/scaling_governor") || { echo balanced; return; }
    epp=$(read_file "$CPU/cpu0/cpufreq/energy_performance_preference") || epp=""
    case $gov in
        performance) echo performance ;;
        powersave)
            case $epp in
                power|balance_power) echo power-saver ;;
                performance)         echo performance ;;
                "")                  echo power-saver ;;   # no EPP: plain powersave
                *)                   echo balanced ;;
            esac ;;
        *) echo balanced ;;                                # schedutil, ondemand...
    esac
}

# ---------------------------------------------------------------------------
write_sysfs() {   # file value
    if printf '%s\n' "$2" >"$1" 2>/dev/null; then return 0; fi
    log "warning: cannot write '$2' to $1"
    return 1
}

apply_profile() {
    local prof=$1
    local -a gov_c epp_c plat_c

    case $prof in
        power-saver) gov_c=(powersave);                  epp_c=(power balance_power);
                     plat_c=(low-power quiet cool) ;;
        balanced)    gov_c=(powersave schedutil ondemand); epp_c=(balance_performance default balance_power);
                     plat_c=(balanced) ;;
        performance) gov_c=(performance);                epp_c=(performance);
                     plat_c=(performance) ;;
    esac

    local ncpu=0 nok=0 d avail g e

    for d in "$CPU"/cpu[0-9]*/cpufreq; do
        [[ -d $d ]] || continue            # offline CPUs have no cpufreq dir
        ncpu=$((ncpu + 1))
        local ok=1

        # governor first (EPP can only be changed outside "performance")
        if [[ -w $d/scaling_governor ]]; then
            avail=$(read_file "$d/scaling_available_governors") || avail=""
            if [[ -n $avail ]]; then
                if g=$(pick "$avail" "${gov_c[@]}") && [[ -n $g ]]; then
                    write_sysfs "$d/scaling_governor" "$g" || ok=0
                else
                    log "warning: no suitable governor in '$avail' for $d"; ok=0
                fi
            fi
        else
            ok=0
        fi

        # energy performance preference (intel_pstate / amd-pstate)
        if [[ -w $d/energy_performance_preference && $prof != performance ]]; then
            avail=$(read_file "$d/energy_performance_available_preferences") || avail=""
            if [[ -n $avail ]] && e=$(pick "$avail" "${epp_c[@]}") && [[ -n $e ]]; then
                write_sysfs "$d/energy_performance_preference" "$e" || ok=0
            fi
        fi
        (( ok )) && nok=$((nok + 1))
    done

    local plat_state="not available"
    if [[ -w $PLATFORM ]]; then
        avail=$(read_file "${PLATFORM}_choices") || avail=""
        if [[ -n $avail ]] && g=$(pick "$avail" "${plat_c[@]}") && [[ -n $g ]]; then
            if write_sysfs "$PLATFORM" "$g"; then plat_state="set to $g"; else plat_state="write failed"; fi
        else
            plat_state="no matching choice in '$avail'"
        fi
    fi

    log "profile '$prof': $nok/$ncpu CPUs configured; platform_profile: $plat_state"
    if (( ncpu == 0 )) && [[ $plat_state != "set to"* ]]; then
        die "no cpufreq or platform_profile interface found in sysfs"
    fi
    (( ncpu > 0 && nok < ncpu )) && return 1
    return 0
}

# ---------------------------------------------------------------------------
[[ $# -ge 1 ]] || usage

if [[ ${1,,} == get ]]; then
    current_profile
    exit 0
fi

prof=$(normalize "$1") || { log "unknown profile '$1'"; usage; }

if (( EUID != 0 )); then
    command -v sudo >/dev/null 2>&1 || die "root is required and sudo is not available"
    exec sudo -n -- "$(readlink -f "$0")" "$prof"
fi

apply_profile "$prof"

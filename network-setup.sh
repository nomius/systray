#!/usr/bin/env bash
#
# network-setup.sh - helper for the systray network widget.
#
#   network-setup.sh WIFI <interface> <essid> <access-point-address> <encryption>
#       encryption: open | wep | wpa | wpa2 | wpa3
#       The password / WEP key (needed for new or changed wep/wpa* networks)
#       comes from the environment variable WIFI_PASSWORD.  Uses wpa_cli to add
#       the network to wpa_supplicant (or update its secret if the ESSID already
#       exists), selects it and saves the configuration.
#       open = no encryption; wep key = 5/13 chars or 10/26 hex digits.
#
#   network-setup.sh REBIND <interface>
#       Asks the DHCP client to renew the lease: sudo dhcpcd -n <interface>
#
#   network-setup.sh DNS <dhcp|cloudflare|google|custom> [interface]
#       dhcp       -> sudo dhcpcd -n <interface> (dhcpcd rewrites resolv.conf
#                     if its resolv.conf hook is enabled); without an
#                     interface nothing is changed
#       cloudflare -> /etc/resolv.conf: 1.1.1.1, 1.0.0.1
#       google     -> /etc/resolv.conf: 8.8.8.8, 8.8.4.4
#       custom     -> /etc/resolv.conf: 127.0.0.1 (run your own local resolver)
#
# Environment (optional):
#   WIFI_PASSWORD  WPA/WPA2/WPA3 passphrase (8..63 chars) or WEP key; never logged
#   WPA_CTRL       wpa_supplicant control socket directory (wpa_cli -p)
#   PIN_BSSID=1    lock the network to the given access-point address
#   RESOLV_CONF    file to write instead of /etc/resolv.conf
#
# Requirements: wpa_supplicant with "update_config=1" in its configuration
# (otherwise save_config cannot write /etc/wpa_supplicant.conf), and permission
# to use wpa_cli (root or the group in ctrl_interface_group, e.g. netdev).
# Writing /etc/resolv.conf needs root; when not root, "sudo -n tee" is used.

set -u -o pipefail

# wpa_cli, dhcpcd, ... live in the sbin directories, which an unprivileged
# user's PATH usually lacks.
for d in /sbin /usr/sbin /usr/local/sbin; do
    case ":$PATH:" in *":$d:"*) ;; *) PATH="$PATH:$d" ;; esac
done
export PATH

PROG=${0##*/}
log() { logger -t network-setup "$PROG $*"; }
die() { log "ERROR: $*"; exit 1; }

usage() {
    cat >&2 <<EOF
usage: $PROG WIFI <interface> <essid> <address> <open|wep|wpa|wpa2|wpa3>   (secret in \$WIFI_PASSWORD)
       $PROG DNS  <dhcp|cloudflare|google|custom> [interface]
       $PROG REBIND <interface>
EOF
    exit 2
}

# ---------------------------------------------------------------------------
# WIFI
# ---------------------------------------------------------------------------
# Store the secret of network $1 for encryption type $2 (key in $3).
# wep : 5/13 ASCII characters or 10/26 hex digits;  wpa*: 8..63 characters
set_secret() {
    local id=$1 enc=$2 key=$3
    case $enc in
        wep)
            if [[ $key =~ ^([0-9A-Fa-f]{10}|[0-9A-Fa-f]{26})$ ]]; then
                wc_ok set_network "$id" wep_key0 "$key"
            elif (( ${#key} == 5 || ${#key} == 13 )); then
                wc_ok set_network "$id" wep_key0 "\"$key\""
            else
                log "WEP key must be 5/13 characters or 10/26 hex digits"
                return 1
            fi ;;
        wpa|wpa2)
            wc_ok set_network "$id" psk "\"$key\"" ;;
        wpa3)
            wc_ok set_network "$id" psk "\"$key\"" || return 1
            wc_ok set_network "$id" sae_password "\"$key\"" || true ;;
    esac
}

# Settings of a newly created network block for encryption type $2.
configure_new() {
    local id=$1 enc=$2
    case $enc in
        open) wc_ok set_network "$id" key_mgmt NONE ;;
        wep)  wc_ok set_network "$id" key_mgmt NONE &&
              wc_ok set_network "$id" wep_tx_keyidx 0 &&
              { wc_ok set_network "$id" auth_alg "OPEN SHARED" || true; } ;;
        wpa)  wc_ok set_network "$id" key_mgmt WPA-PSK &&
              wc_ok set_network "$id" proto WPA &&
              { wc_ok set_network "$id" pairwise "TKIP CCMP" || true; } &&
              { wc_ok set_network "$id" group "TKIP CCMP" || true; } ;;
        wpa2) wc_ok set_network "$id" key_mgmt WPA-PSK &&
              wc_ok set_network "$id" proto RSN ;;
        wpa3) wc_ok set_network "$id" key_mgmt SAE &&
              wc_ok set_network "$id" ieee80211w 2 ;;
    esac
}

wifi_connect() {
    local iface=${1:-} essid=${2:-} bssid=${3:-} enc=${4:-} pw=${WIFI_PASSWORD:-}

    [[ -n $iface && -n $essid ]] || usage
    [[ $iface =~ ^[A-Za-z0-9._-]+$ ]] || die "invalid interface name '$iface'"
    command -v wpa_cli >/dev/null 2>&1 || die "wpa_cli not found"

    enc=${enc,,}
    if [[ -z $enc ]]; then                       # older callers: guess from the password
        [[ -n $pw ]] && enc=wpa2 || enc=open
    fi
    case $enc in
        open|wep|wpa|wpa2|wpa3) ;;
        *) die "unknown encryption type '$enc' (open|wep|wpa|wpa2|wpa3)" ;;
    esac
    [[ $enc == open ]] && pw=""

    case $enc in
        wpa|wpa2|wpa3)
            if [[ -n $pw ]] && (( ${#pw} < 8 || ${#pw} > 63 )); then
                die "WIFI_PASSWORD must be 8..63 characters for $enc"
            fi ;;
    esac

    local -a WC=(wpa_cli)
    [[ -n ${WPA_CTRL:-} ]] && WC+=(-p "$WPA_CTRL")
    WC+=(-i "$iface")

    # run a wpa_cli command that must answer OK
    wc_ok() {
        local out
        out=$("${WC[@]}" "$@" 2>&1)
        [[ ${out##*$'\n'} == OK ]]
    }

    log "interface=$iface essid='$essid' address=${bssid:-none} encryption=$enc password=$([[ -n $pw ]] && echo provided || echo none)"

    local list
    list=$("${WC[@]}" list_networks 2>&1) || die "wpa_cli list_networks failed: $list"
    [[ $list == *"Failed to connect"* ]] && die "cannot reach wpa_supplicant on $iface: $list"

    # look for an existing network block with this ESSID
    local id="" nid nssid rest
    while IFS=$'\t' read -r nid nssid rest; do
        [[ $nid =~ ^[0-9]+$ ]] || continue
        if [[ $nssid == "$essid" ]]; then id=$nid; break; fi
    done <<<"$list"

    local created=0
    if [[ -n $id ]]; then
        log "network '$essid' already configured (id $id)"
    else
        id=$("${WC[@]}" add_network 2>&1)
        [[ $id =~ ^[0-9]+$ ]] || die "add_network failed: $id"
        created=1
        log "added network id $id"

        local hex
        hex=$(printf '%s' "$essid" | od -An -v -tx1 | tr -d ' \n')   # any characters
        if ! wc_ok set_network "$id" ssid "$hex" || ! configure_new "$id" "$enc"; then
            "${WC[@]}" remove_network "$id" >/dev/null 2>&1
            die "configuring network '$essid' as $enc failed (does this wpa_supplicant support $enc?)"
        fi
    fi

    # secret (new network or changed password / key of an existing one)
    if [[ -n $pw ]]; then
        if ! set_secret "$id" "$enc" "$pw"; then
            (( created )) && "${WC[@]}" remove_network "$id" >/dev/null 2>&1
            die "storing the $enc secret failed (is the password/key valid?)"
        fi
        log "$enc secret set for network $id"
    elif (( created )) && [[ $enc != open ]]; then
        "${WC[@]}" remove_network "$id" >/dev/null 2>&1
        die "network '$essid' uses $enc but no WIFI_PASSWORD was given"
    fi

    if [[ ${PIN_BSSID:-0} == 1 && -n $bssid ]]; then
        wc_ok set_network "$id" bssid "$bssid" || log "warning: could not set bssid $bssid"
    fi

    wc_ok enable_network "$id" || die "enable_network $id failed"
    wc_ok select_network "$id" || die "select_network $id failed"
    log "selected network $id ('$essid')"

    if wc_ok save_config; then
        log "configuration saved"
    else
        log "warning: save_config failed (add 'update_config=1' to wpa_supplicant.conf)"
    fi
}
# ---------------------------------------------------------------------------
# DNS
# ---------------------------------------------------------------------------
dns_set() {
    local provider=${1:-}
    local -a ns

    case $provider in
        dhcp)
            # let dhcpcd rewrite resolv.conf with the DHCP-provided servers
            if [[ -n ${2:-} ]]; then
                log "DHCP selected: renewing lease on $2"
                rebind_ip "$2"
            else
                log "DHCP selected but no interface given: leaving DNS configuration untouched"
            fi
            return 0 ;;
        cloudflare) ns=(1.1.1.1 1.0.0.1) ;;
        google)     ns=(8.8.8.8 8.8.4.4) ;;
        custom)     ns=(127.0.0.1) ;;
        *)          usage ;;
    esac

    local resolv=${RESOLV_CONF:-/etc/resolv.conf}
    local content="# Generated by $PROG ($provider)"$'\n'

    # keep search/domain/options lines of the current file
    if [[ -r $resolv ]]; then
        local keep
        keep=$(grep -E '^(search|domain|options)[[:space:]]' "$resolv" 2>/dev/null || true)
        [[ -n $keep ]] && content+="$keep"$'\n'
    fi
    local s
    for s in "${ns[@]}"; do content+="nameserver $s"$'\n'; done

    [[ -L $resolv ]] && log "note: $resolv is a symlink to $(readlink -f "$resolv"); a DHCP/resolver manager may overwrite it"

    if [[ -w $resolv || ( ! -e $resolv && -w ${resolv%/*} ) ]]; then
        printf '%s' "$content" >"$resolv" || die "cannot write $resolv"
    elif command -v sudo >/dev/null 2>&1; then
        printf '%s' "$content" | sudo -n tee "$resolv" >/dev/null ||
            die "cannot write $resolv (run as root or allow passwordless sudo tee)"
    else
        die "cannot write $resolv (not root and sudo not available)"
    fi
    log "$resolv set to: ${ns[*]}"
}

# ---------------------------------------------------------------------------
# REBIND
# ---------------------------------------------------------------------------
rebind_ip() {
    local iface=${1:-}
    [[ -n $iface ]] || usage
    # must not start with "-": "-cfoo" would make dhcpcd run ./foo as root
    [[ $iface =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] || die "invalid interface name '$iface'"

    # Not root: re-run only this step through sudo, so sudoers can allow
    # exactly "network-setup.sh REBIND *" (see kwort-systray.sudoers) and the
    # interface name is checked above, as root, before dhcpcd sees it.
    if (( EUID != 0 )); then
        command -v sudo >/dev/null 2>&1 || die "root is required and sudo is not available"
        sudo -n -- "$(readlink -f "$0")" REBIND "$iface" ||
            die "'sudo -n $(readlink -f "$0") REBIND $iface' failed (see kwort-systray.sudoers)"
        return 0
    fi

    command -v dhcpcd >/dev/null 2>&1 || die "dhcpcd not found"
    log "rebinding IP on $iface: dhcpcd -n $iface"
    dhcpcd -n -- "$iface" || die "dhcpcd -n $iface failed"
    log "dhcpcd rebind requested for $iface"
}

# ---------------------------------------------------------------------------
case ${1:-} in
    WIFI) shift; wifi_connect "$@" ;;
    DNS)  shift; dns_set "$@" ;;
    REBIND) shift; rebind_ip "$@" ;;
    *)    usage ;;
esac

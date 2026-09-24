#!/bin/sh
# RNDIS USB DHCP for PC/router: ConnMan tether + udhcpd fallback (patch v3)

# Re-exec under nohup so adb shell session exit does not SIGHUP-kill daemon
if [ -z "$USB_TETHER_NOHUP" ]; then
    setsid env USB_TETHER_NOHUP=1 /home/root/usb-tether.sh >> /tmp/usb-tether.log 2>&1 &
    exit 0
fi

LOCK=/tmp/usb-tether.pid
LOG=/tmp/usb-tether.log
LOG_SINK="$LOG"
HEAL_LOG=/tmp/usb-tether-heal.log
REPAIR_MIN_INTERVAL=60
DEEP_RECOVER_FAILS=6
UDC_RESET_FAILS=3
UNHEALTHY_ROUNDS_BEFORE_REPAIR=3
# PC/router phantom: gw=1 but no ARP neigh / lease (ImmortalWrt static often has no lease)
PC_PHANTOM_MISS_STREAK=24
PC_PHANTOM_COOLDOWN_S=900
PC_PHANTOM_DAY_MAX=4
PC_PHANTOM_BOOT_GRACE_S=120
PC_PHANTOM_STATE=/tmp/usb-tether-pc-phantom.state
pc_phantom_miss=0
pc_phantom_boot_ts=0
USING_UDHCPD=0
MIGRATE_ATTEMPTED=0
last_repair_ts=0
fail_streak=0
unhealthy_streak=0

log_msg() {
    echo "$(date '+%F %T') $*" >> "$LOG" 2>/dev/null
    echo "$(date '+%F %T') $*" >> "$HEAL_LOG" 2>/dev/null
    # keep heal log small
    if [ -f "$HEAL_LOG" ]; then
        sz=$(wc -c < "$HEAL_LOG" 2>/dev/null || echo 0)
        if [ "$sz" -gt 200000 ]; then
            tail -n 200 "$HEAL_LOG" > "$HEAL_LOG.tmp" 2>/dev/null && mv "$HEAL_LOG.tmp" "$HEAL_LOG"
        fi
    fi
}

spawn_usb_share_at() {
    reason="$1"
    if [ -x /home/root/enable-usb-share-at.sh ]; then
        setsid /home/root/enable-usb-share-at.sh "$reason" &
    fi
}

init_log() {
    echo "$(date '+%F %T') boot session start pid=$$" >> "$LOG" 2>/dev/null
}

# Only one daemon (take over stale pid file on restart)
if [ -f "$LOCK" ]; then
    oldpid=$(cat "$LOCK" 2>/dev/null)
    if [ -n "$oldpid" ] && [ "$oldpid" != "$$" ]; then
        if kill -0 "$oldpid" 2>/dev/null; then
            exit 0
        fi
        rm -f "$LOCK"
    fi
fi
echo $$ > "$LOCK"

usb0_ready() {
    ip link show usb0 >/dev/null 2>&1 || return 1
    ip link set dev usb0 up 2>/dev/null
    state=$(cat /sys/class/net/usb0/operstate 2>/dev/null)
    carrier=$(cat /sys/class/net/usb0/carrier 2>/dev/null)
    [ "$state" = "up" ] || [ "$state" = "unknown" ] || [ "$carrier" = "1" ]
}

# Wait until usb0 can hold 192.168.66.1 (avoids early boot "Cannot assign" + USB thrash)
wait_usb0_addr_ok() {
    max="${1:-20}"
    n=0
    while [ "$n" -lt "$max" ]; do
        usb0_ready || { sleep 1; n=$((n + 1)); continue; }
        ip addr add 192.168.66.1/24 dev usb0 2>/dev/null
        if has_gateway_ip; then
            return 0
        fi
        sleep 1
        n=$((n + 1))
    done
    return 1
}

# Bind gateway IP without flapping link (link down causes ImmortalWrt "接口不可用")
ensure_usb0_gateway_ip() {
    ip link set dev usb0 up 2>/dev/null
    if has_gateway_ip; then
        return 0
    fi
    # Drop other addresses only; do not link-down
    ip addr flush dev usb0 2>/dev/null
    ip addr add 192.168.66.1/24 dev usb0 2>/dev/null
    has_gateway_ip
}

ensure_connmand() {
    ps | grep -q '[c]onnmand' && return 0
    if [ -x /home/root/6677/connmand ]; then
        /home/root/6677/connmand -n -d >> "$LOG_SINK" 2>&1 &
    elif [ -x /usr/sbin/connmand ]; then
        /usr/sbin/connmand -n -d >> "$LOG_SINK" 2>&1 &
    fi
    sleep 1
    ps | grep -q '[c]onnmand'
}

dhcp_listening() {
    netstat -ulnp 2>/dev/null | grep -E ':67[[:space:]]' | grep -Eq 'connmand|udhcpd|dnsmasq' && return 0
    netstat -uln 2>/dev/null | grep -q ':67'
}

has_gateway_ip() {
    ifconfig usb0 2>/dev/null | grep -q 'inet addr:192.168.66.1'
}

connman_tether_true() {
    out=$(connmanctl technologies 2>&1 | grep -A6 gadget)
    echo "$out" | grep -Eiq 'Tethering[[:space:]]*=[[:space:]]*True'
}

udhcpd_running() {
    ps | grep -q '[u]dhcpd'
}

# udhcpd actually serving DHCP (not ConnMan still on :67)
udhcpd_serving_dhcp() {
    udhcpd_running || return 1
    connman_tether_true && return 1
    has_gateway_ip || return 1
    netstat -ulnp 2>/dev/null | grep -E ':67[[:space:]]' | grep -q '[u]dhcpd' && return 0
    netstat -uln 2>/dev/null | grep -q ':67'
}

tether_runtime_ok() {
    has_gateway_ip && dhcp_listening
}

tether_health_ok() {
    tether_runtime_ok || return 1
    if [ "$USING_UDHCPD" = "1" ]; then
        return 0
    fi
    if udhcpd_running; then
        USING_UDHCPD=1
        return 0
    fi
    connman_tether_true
}

# Opt-in IPv6 NAT66 for USB tether (marker default off). Failures never fail IPv4.
ensure_ipv6_nat66() {
    [ -f /mnt/data/usb-ipv6-nat66.enable ] || return 0

    uplink=rmnet_data0
    [ -d /sys/class/net/ccinet0 ] && uplink=ccinet0
    [ -d /sys/class/net/sipa_eth0 ] && uplink=sipa_eth0

    if [ ! -d "/sys/class/net/$uplink" ]; then
        log_msg "ipv6-nat66: no uplink ($uplink), skip"
        return 0
    fi
    if ! command -v ip6tables >/dev/null 2>&1; then
        log_msg "ipv6-nat66: ip6tables missing, skip"
        return 0
    fi

    if ! echo 1 > /proc/sys/net/ipv6/conf/all/forwarding 2>/dev/null; then
        log_msg "ipv6-nat66: set all/forwarding failed"
        return 0
    fi
    if ! echo 1 > /proc/sys/net/ipv6/conf/usb0/forwarding 2>/dev/null; then
        log_msg "ipv6-nat66: set usb0/forwarding failed"
        return 0
    fi
    if ! echo 1 > /proc/sys/net/ipv6/conf/"$uplink"/forwarding 2>/dev/null; then
        log_msg "ipv6-nat66: set $uplink/forwarding failed"
        return 0
    fi
    if ! echo 0 > /proc/sys/net/ipv6/conf/usb0/accept_ra 2>/dev/null; then
        log_msg "ipv6-nat66: set usb0/accept_ra=0 failed"
        return 0
    fi

    if ! ip -6 addr replace fd66:6677::1/64 dev usb0 2>/dev/null; then
        log_msg "ipv6-nat66: usb0 ULA fd66:6677::1/64 failed"
        return 0
    fi

    if ! ip6tables -t nat -C POSTROUTING -o "$uplink" -j MASQUERADE 2>/dev/null; then
        if ! ip6tables -t nat -A POSTROUTING -o "$uplink" -j MASQUERADE 2>/dev/null; then
            log_msg "ipv6-nat66: MASQUERADE -o $uplink failed"
            return 0
        fi
    fi
    if ! ip6tables -C FORWARD -i usb0 -o "$uplink" -j ACCEPT 2>/dev/null; then
        if ! ip6tables -A FORWARD -i usb0 -o "$uplink" -j ACCEPT 2>/dev/null; then
            log_msg "ipv6-nat66: FORWARD usb0->$uplink failed"
            return 0
        fi
    fi
    if ! ip6tables -C FORWARD -i "$uplink" -o usb0 -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null; then
        if ! ip6tables -A FORWARD -i "$uplink" -o usb0 -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null; then
            log_msg "ipv6-nat66: FORWARD $uplink->usb0 failed"
            return 0
        fi
    fi

    log_msg "ipv6-nat66: ok uplink=$uplink"
    return 0
}

ensure_udhcpd_nat() {
    uplink=rmnet_data0
    [ -d /sys/class/net/ccinet0 ] && uplink=ccinet0
    [ -d /sys/class/net/sipa_eth0 ] && uplink=sipa_eth0
    iptables -t nat -C POSTROUTING -o "$uplink" -j MASQUERADE 2>/dev/null || \
        iptables -t nat -A POSTROUTING -o "$uplink" -j MASQUERADE 2>/dev/null
    iptables -C FORWARD -i usb0 -j ACCEPT 2>/dev/null || \
        iptables -A FORWARD -i usb0 -j ACCEPT 2>/dev/null
    ensure_ipv6_nat66
}

cellular_service_id() {
    connmanctl services 2>&1 | sed -n '/^\*AR /p' | awk '{print $NF}' | sed -n '1p'
}

get_gsm_dns_v4() {
    svc=$(cellular_service_id)
    [ -z "$svc" ] && svc=$(connmanctl services 2>&1 | grep context1 | awk '{print $NF}' | sed -n '1p')
    [ -z "$svc" ] && return 1

    ns_line=$(connmanctl services "$svc" 2>&1 | grep 'Nameservers =')
    [ -z "$ns_line" ] && return 1

    list=$(echo "$ns_line" | sed 's/.*Nameservers = \[ //;s/ \]$//')
    OLDIFS="$IFS"
    IFS=,
    for ip in $list; do
        ip=$(echo "$ip" | tr -d ' ')
        echo "$ip" | grep -q ':' && continue
        [ -n "$ip" ] && echo "$ip"
    done
    IFS="$OLDIFS"
}

wait_gsm_dns_v4() {
    n=0
    while [ "$n" -lt "$1" ]; do
        if get_gsm_dns_v4 | grep -q .; then
            return 0
        fi
        sleep 1
        n=$((n + 1))
    done
    return 1
}

load_cached_gsm_dns_v4() {
    [ -f /mnt/data/last-gsm-dns.csv ] || return 1
    tr ',' '\n' < /mnt/data/last-gsm-dns.csv | grep -v '^$'
}

save_gsm_dns_cache() {
    _csv=$(gsm_dns_csv)
    [ -n "$_csv" ] && echo "$_csv" > /mnt/data/last-gsm-dns.csv
}

gsm_dns_csv() {
    _csv=$(get_gsm_dns_v4 | tr '\n' ',' | sed 's/,$//')
    echo "$_csv"
}

LAST_DHCP_DNS=

# Reject gateway/loopback as DHCP DNS — device has no local resolver (H3 / ImmortalWrt)
is_usable_dhcp_dns() {
    case "$1" in
        ""|127.*|192.168.66.1|0.0.0.0) return 1 ;;
        *:*) return 1 ;; # skip IPv6 in udhcpd opt dns
        *) return 0 ;;
    esac
}

append_dhcp_dns_ip() {
    is_usable_dhcp_dns "$1" || return 0
    dns_lines="${dns_lines}
opt dns ${1}"
}

write_udhcpd_conf() {
    dns_lines=
    for ip in $(get_gsm_dns_v4); do
        append_dhcp_dns_ip "$ip"
    done
    if [ -z "$dns_lines" ]; then
        for ip in $(load_cached_gsm_dns_v4); do
            append_dhcp_dns_ip "$ip"
        done
    fi
    if [ -z "$dns_lines" ]; then
        # Never advertise device IP as sole DNS — no local resolver (H3)
        dns_lines="
opt dns 114.114.114.114
opt dns 8.8.8.8"
    fi

    cat > /tmp/udhcpd.conf <<EOF
interface usb0
start 192.168.66.10
end 192.168.66.50
max_leases 41
opt subnet 255.255.255.0
opt router 192.168.66.1${dns_lines}
lease_file /var/lib/misc/udhcpd.leases
EOF
}

# After ConnMan→udhcpd migrate: refresh DHCP without USB link flap.
# (ip link down makes ImmortalWrt report usbwan unavailable / NO_DEVICE)
nudge_dhcp_clients_renew() {
    reason="${1:-migrate}"
    log_msg "nudge dhcp clients renew soft ($reason)"
    ensure_usb0_gateway_ip
    # Restart udhcpd only — static ImmortalWrt clients ignore DHCP; DHCP clients renew on next discover
    restart_udhcpd_only
    ensure_udhcpd_nat
    if ! has_gateway_ip || ! dhcp_listening; then
        log_msg "nudge: tether weak, re-enable udhcpd"
        enable_udhcpd
    fi
}

restart_udhcpd_only() {
    write_udhcpd_conf
    [ -f /home/root/kill-orphan-udhcpd.sh ] && sh /home/root/kill-orphan-udhcpd.sh
    sleep 1
    nohup udhcpd -f /tmp/udhcpd.conf >> "$LOG_SINK" 2>&1 &
    sleep 1
}

maybe_refresh_dhcp_dns() {
    new=$(gsm_dns_csv)
    [ -n "$new" ] || return 0
    save_gsm_dns_cache
    new_norm=$(echo "$new" | tr ',' '\n' | grep -v '^$' | sort | tr '\n' ',' | sed 's/,$//')
    last_norm=$(echo "$LAST_DHCP_DNS" | tr ',' '\n' | grep -v '^$' | sort | tr '\n' ',' | sed 's/,$//')
    [ "$new_norm" = "$last_norm" ] && return 0
    log_msg "gsm dns update: ${LAST_DHCP_DNS:-none} -> $new"
    LAST_DHCP_DNS="$new"
    [ "$USING_UDHCPD" = "1" ] || return 0
    restart_udhcpd_only
}

disable_connman_gadget_tether() {
    ps | grep -q '[c]onnmand' || return 0
    # Only turn off if actually tethering — toggling gadget resets USB enumeration
    connman_tether_true || return 0
    log_msg "disable connman gadget tether (was on)"
    connmanctl tether gadget off >> "$LOG_SINK" 2>/dev/null
}

migrate_connman_to_udhcpd() {
    connman_tether_true || return 0
    udhcpd_serving_dhcp && { USING_UDHCPD=1; return 0; }
    [ "$MIGRATE_ATTEMPTED" = "1" ] && return 0

    MIGRATE_ATTEMPTED=1
    log_msg "migrate connman to udhcpd start"

    disable_connman_gadget_tether
    sleep 1
    enable_udhcpd

    if udhcpd_serving_dhcp; then
        USING_UDHCPD=1
        fail_streak=0
        log_msg "migrated connman to udhcpd"
        # Kick clients off ConnMan lease (DNS=192.168.66.1) onto udhcpd DNS
        nudge_dhcp_clients_renew migrate
        return 0
    fi

    log_msg "migrate to udhcpd failed, restore connman"
    [ -f /home/root/kill-orphan-udhcpd.sh ] && sh /home/root/kill-orphan-udhcpd.sh
    sleep 1
    USING_UDHCPD=0
    enable_connman_tether
    if connman_tether_true && has_gateway_ip && dhcp_listening; then
        log_msg "connman restored after migrate rollback"
        return 1
    fi
    log_msg "connman restore weak, retry enable_tether"
    enable_tether
    return 1
}

enable_udhcpd() {
    [ -f /home/root/kill-orphan-udhcpd.sh ] && sh /home/root/kill-orphan-udhcpd.sh
    sleep 1
    ensure_usb0_gateway_ip || {
        log_msg "enable_udhcpd: usb0 addr not ready"
        return 1
    }
    echo 1 > /proc/sys/net/ipv4/ip_forward 2>/dev/null
    mkdir -p /var/lib/misc
    touch /var/lib/misc/udhcpd.leases
    write_udhcpd_conf
    LAST_DHCP_DNS=$(gsm_dns_csv)
    [ -z "$LAST_DHCP_DNS" ] && LAST_DHCP_DNS=$(tr ',' '\n' < /mnt/data/last-gsm-dns.csv 2>/dev/null | grep -v '^$' | tr '\n' ',' | sed 's/,$//')
    if [ -n "$LAST_DHCP_DNS" ]; then
        log_msg "dhcp dns: $LAST_DHCP_DNS"
        save_gsm_dns_cache
    else
        log_msg "dhcp dns fallback: 114.114.114.114,8.8.8.8 (gsm pending)"
    fi
    nohup udhcpd -f /tmp/udhcpd.conf >> "$LOG_SINK" 2>&1 &
    sleep 1
    ensure_udhcpd_nat
}

handle_cellular_refresh() {
    [ -f /tmp/usb-tether-refresh ] || return 1
    rm -f /tmp/usb-tether-refresh
    log_msg "cellular refresh: nat+dns"
    echo 1 > /proc/sys/net/ipv4/ip_forward 2>/dev/null
    ensure_udhcpd_nat
    if [ "$USING_UDHCPD" = "1" ] || udhcpd_running; then
        USING_UDHCPD=1
        # H11: only restart udhcpd when DNS actually changes (avoid PC flicker)
        new=$(gsm_dns_csv)
        if [ -n "$new" ]; then
            save_gsm_dns_cache
            new_norm=$(echo "$new" | tr ',' '\n' | grep -v '^$' | sort | tr '\n' ',' | sed 's/,$//')
            last_norm=$(echo "$LAST_DHCP_DNS" | tr ',' '\n' | grep -v '^$' | sort | tr '\n' ',' | sed 's/,$//')
            if [ "$new_norm" != "$last_norm" ]; then
                log_msg "gsm dns update: ${LAST_DHCP_DNS:-none} -> $new"
                LAST_DHCP_DNS="$new"
                restart_udhcpd_only
            else
                log_msg "cellular refresh: dns unchanged, skip udhcpd restart"
            fi
        else
            log_msg "cellular refresh: gsm dns pending, keep current dhcp"
        fi
        ensure_udhcpd_nat
    fi
    return 0
}

enable_connman_tether() {
    connmanctl enable gadget >> "$LOG_SINK" 2>&1
    connmanctl tether gadget on >> "$LOG_SINK" 2>&1
    echo 1 > /proc/sys/net/ipv4/ip_forward 2>/dev/null
    sleep 1
}

now_s() {
    busybox date +%s 2>/dev/null || date +%s 2>/dev/null
}

should_repair_now() {
    now=$(now_s)
    [ -n "$now" ] || return 0
    [ "$now" -ge "$((last_repair_ts + REPAIR_MIN_INTERVAL))" ]
}

mark_repair() {
    now=$(now_s)
    [ -n "$now" ] && last_repair_ts="$now"
}

udc_soft_reset() {
    udc=$(ls /sys/class/udc 2>/dev/null | sed -n '1p')
    [ -z "$udc" ] && udc=29100000.dwc3
    log_msg "udc soft reset: $udc"
    echo none > /sys/kernel/config/usb_gadget/g1/UDC 2>/dev/null
    sleep 1
    echo "$udc" > /sys/kernel/config/usb_gadget/g1/UDC 2>/dev/null
    sleep 2
}

# True if ImmortalWrt/PC looks alive on 192.168.66.0/24 (neigh, ARP, lease)
pc_phantom_peer_alive() {
    # Ops probe: force miss path without unplugging ImmortalWrt
    [ -f /tmp/usb-tether-force-phantom ] && return 1

    if ip neigh show dev usb0 2>/dev/null | grep -E '192\.168\.66\.[0-9]+' | grep -Eqv 'FAILED|INCOMPLETE'; then
        return 0
    fi
    # BusyBox often lacks `ip neigh`; v1.27.2 awk regex can segfault — use grep
    if [ -f /proc/net/arp ]; then
        if grep '192.168.66.' /proc/net/arp 2>/dev/null | grep ' usb0' | grep -qv '00:00:00:00:00:00'; then
            return 0
        fi
    fi
    _lf=/var/lib/misc/udhcpd.leases
    [ -f "$_lf" ] || _lf=/tmp/udhcpd.leases
    if [ -f "$_lf" ] && grep -Eq '192\.168\.66\.(1[0-9]|[2-4][0-9]|50)([[:space:]]|$)' "$_lf" 2>/dev/null; then
        return 0
    fi
    return 1
}

pc_phantom_load_day() {
    _day=$(date +%Y%m%d 2>/dev/null)
    _count=0
    _last=0
    if [ -f "$PC_PHANTOM_STATE" ]; then
        # format: DAY COUNT LAST_TS
        read -r _d _c _l < "$PC_PHANTOM_STATE" 2>/dev/null || true
        if [ "$_d" = "$_day" ]; then
            _count=${_c:-0}
            _last=${_l:-0}
        fi
    fi
}

pc_phantom_save_day() {
    _day=$(date +%Y%m%d 2>/dev/null)
    echo "$_day $1 $2" > "$PC_PHANTOM_STATE" 2>/dev/null
}

pc_phantom_tick() {
    has_gateway_ip || return 0

    _now=$(now_s)
    [ -z "$_now" ] && return 0

    # Boot grace: wait for ImmortalWrt ARP
    if [ "$pc_phantom_boot_ts" -gt 0 ] && [ "$_now" -lt $((pc_phantom_boot_ts + PC_PHANTOM_BOOT_GRACE_S)) ]; then
        return 0
    fi

    if pc_phantom_peer_alive; then
        if [ "$pc_phantom_miss" -gt 0 ]; then
            log_msg "pc-phantom: peer seen, streak cleared"
        fi
        pc_phantom_miss=0
        return 0
    fi

    pc_phantom_miss=$((pc_phantom_miss + 1))
    log_msg "pc-phantom: miss streak=$pc_phantom_miss/$PC_PHANTOM_MISS_STREAK (no neigh/lease)"

    [ "$pc_phantom_miss" -lt "$PC_PHANTOM_MISS_STREAK" ] && return 0

    pc_phantom_load_day
    if [ "$_count" -ge "$PC_PHANTOM_DAY_MAX" ]; then
        log_msg "pc-phantom: skip daily budget"
        pc_phantom_miss=0
        return 0
    fi
    if [ "$_last" -gt 0 ] && [ "$_now" -lt $((_last + PC_PHANTOM_COOLDOWN_S)) ]; then
        log_msg "pc-phantom: skip cooldown"
        pc_phantom_miss=0
        return 0
    fi

    _next=$((_count + 1))
    log_msg "pc-phantom: udc reset (day $_next/$PC_PHANTOM_DAY_MAX)"
    udc_soft_reset
    enable_tether || true
    mark_repair
    pc_phantom_save_day "$_next" "$_now"
    pc_phantom_miss=0
}

# Soft reboot (cable stays in) needs UDC to wake host RNDIS (H9).
# Physical unplug/replug already re-enumerates — UDC here breaks Windows "Identifying" (H11).
# Marker /mnt/data/need-usb-renum is set by crontab / device_reboot before reboot.
warm_reboot_usb_recover() {
    if [ ! -f /mnt/data/need-usb-renum ]; then
        # #region agent log
        log_msg "skip udc reset: physical plug or loader restart (H11)"
        # #endregion
        sleep 2
        spawn_usb_share_at boot-no-udc
        return 0
    fi
    rm -f /mnt/data/need-usb-renum
    # #region agent log
    log_msg "warm reboot usb recover: udc reset (H9 marker)"
    # #endregion
    [ -x /home/root/fix-rndis-link.sh ] && /home/root/fix-rndis-link.sh >> "$LOG" 2>&1

    # ImmortalWrt hosts often need 2–3 UDC cycles after soft reboot (TX watchdog).
    _try=0
    while [ "$_try" -lt 3 ]; do
        _try=$((_try + 1))
        log_msg "warm reboot udc try $_try/3"
        udc_soft_reset
        sleep 2
        spawn_usb_share_at "warm-reboot-$_try"
        if wait_usb0_addr_ok 20; then
            log_msg "warm reboot: usb0 addr ok on try $_try"
            return 0
        fi
        log_msg "warm reboot: usb0 addr miss try $_try"
        sleep 3
    done
    log_msg "warm reboot: usb0 addr wait exhausted after 3 udc cycles"
}

deep_recover() {
    log_msg "deep recover: restart connmand/udhcpd"
    [ -f /home/root/kill-orphan-udhcpd.sh ] && sh /home/root/kill-orphan-udhcpd.sh
    sleep 1
    killall connmand 2>/dev/null
    sleep 2
    USING_UDHCPD=0
    ensure_connmand >/dev/null 2>&1
}

enable_tether() {
    usb0_ready || return 1

    # Prefer udhcpd (correct DNS). Wait for usb0 to accept IP before first try.
    if ! has_gateway_ip; then
        log_msg "waiting usb0 for 192.168.66.1"
        wait_usb0_addr_ok 15 || log_msg "usb0 addr wait timeout, continue"
    fi

    _try=0
    while [ "$_try" -lt 5 ]; do
        enable_udhcpd
        if udhcpd_serving_dhcp; then
            USING_UDHCPD=1
            log_msg "udhcpd ok"
            # Do NOT toggle ConnMan gadget here — resets USB and drops ImmortalWrt usbwan
            spawn_usb_share_at tether-ok
            return 0
        fi
        _try=$((_try + 1))
        if [ "$_try" -lt 5 ]; then
            log_msg "udhcpd not ready, retry $_try/5"
            wait_usb0_addr_ok 8 || true
            sleep 3
        fi
    done

    ensure_connmand
    if ps | grep -q '[c]onnmand'; then
        [ -f /home/root/kill-orphan-udhcpd.sh ] && sh /home/root/kill-orphan-udhcpd.sh
        USING_UDHCPD=0
        enable_connman_tether
        if has_gateway_ip && dhcp_listening && connman_tether_true; then
            USING_UDHCPD=0
            # ConnMan tether typically advertises gateway as DNS (no resolver on device)
            log_msg "connman tether ok (DNS may be 192.168.66.1 until migrate)"
            spawn_usb_share_at tether-ok
            return 0
        fi
        # Use guarded helper — bare "gadget off" resets USB even when tether was never True
        disable_connman_gadget_tether
        sleep 2
        enable_udhcpd
        if udhcpd_serving_dhcp; then
            USING_UDHCPD=1
            log_msg "udhcpd fallback ok"
            spawn_usb_share_at tether-ok
            return 0
        fi
    fi
    return 1
}

sync_backend_state() {
    if udhcpd_running; then
        USING_UDHCPD=1
    elif connman_tether_true; then
        USING_UDHCPD=0
    fi
}

init_log
pc_phantom_boot_ts=$(now_s)
[ -z "$pc_phantom_boot_ts" ] && pc_phantom_boot_ts=0
log_msg "usb-tether daemon start (rndis v3 stable-usb)"
sync_backend_state

# H9 only when soft-reboot marker set; H11 skip UDC on physical plug
warm_reboot_usb_recover

if wait_usb0_addr_ok 25; then
    log_msg "usb0 gateway ready before tether"
else
    if ! usb0_ready; then
        log_msg "usb0 missing, fallback udc once"
        [ -x /home/root/fix-rndis-link.sh ] && /home/root/fix-rndis-link.sh >> "$LOG" 2>&1
        udc_soft_reset
        wait_usb0_addr_ok 20 || log_msg "usb0 still not ready after fallback udc"
    else
        log_msg "usb0 up but no IP yet, tether will retry (no udc — H11)"
    fi
fi

if enable_tether; then
    log_msg "initial tether ok"
fi

last_carrier=
n=0
while true; do
    n=$((n + 1))
    if [ -f /tmp/usb-tether-force-repair ]; then
        rm -f /tmp/usb-tether-force-repair
        log_msg "force repair (outage-watch)"
        if enable_tether; then
            fail_streak=0
            unhealthy_streak=0
            mark_repair
            log_msg "force repair ok (round $n)"
        else
            fail_streak=$((fail_streak + 1))
            mark_repair
            log_msg "force repair failed streak=$fail_streak (round $n)"
        fi
    fi
    handle_cellular_refresh
    if usb0_ready; then
        carrier=$(cat /sys/class/net/usb0/carrier 2>/dev/null)
        if [ "$carrier" = "0" ]; then
            [ "$last_carrier" != "0" ] && log_msg "usb0 carrier down"
            last_carrier=0
        elif [ "$last_carrier" = "0" ]; then
            log_msg "usb0 carrier up, re-tether"
            if enable_tether; then
                fail_streak=0
                unhealthy_streak=0
                mark_repair
                spawn_usb_share_at carrier-up
                log_msg "tether ready after link up (round $n)"
            fi
            last_carrier=1
        else
            last_carrier=1
        fi

        if ! tether_runtime_ok; then
            unhealthy_streak=$((unhealthy_streak + 1))
            if [ "$unhealthy_streak" -ge "$UNHEALTHY_ROUNDS_BEFORE_REPAIR" ] && should_repair_now; then
                log_msg "runtime down, repair (round $n)"
                if enable_tether; then
                    unhealthy_streak=0
                    fail_streak=0
                    mark_repair
                    log_msg "tether ready (round $n)"
                else
                    fail_streak=$((fail_streak + 1))
                    mark_repair
                    log_msg "repair failed streak=$fail_streak (round $n)"
                fi
            fi
        else
            unhealthy_streak=0
            maybe_refresh_dhcp_dns
            migrate_connman_to_udhcpd
        fi

        if [ "$fail_streak" -ge "$UDC_RESET_FAILS" ] && [ "$fail_streak" -lt "$DEEP_RECOVER_FAILS" ] && should_repair_now; then
            udc_soft_reset
            mark_repair
            if enable_tether; then
                fail_streak=0
                log_msg "tether recovered after udc reset (round $n)"
            fi
        fi

        if [ "$fail_streak" -ge "$DEEP_RECOVER_FAILS" ] && should_repair_now; then
            deep_recover
            mark_repair
            if enable_tether; then
                fail_streak=0
                log_msg "tether recovered after deep recover (round $n)"
            fi
        fi

        # Option-2: gw=1 but ImmortalWrt/PC phantom → rate-limited UDC
        pc_phantom_tick
    else
        [ -n "$last_carrier" ] && log_msg "usb0 missing or down (round $n)"
        last_carrier=
        unhealthy_streak=$((unhealthy_streak + 1))
        if [ "$unhealthy_streak" -ge 2 ] && should_repair_now; then
            log_msg "usb0 absent, rndis recover (round $n)"
            [ -x /home/root/fix-rndis-link.sh ] && /home/root/fix-rndis-link.sh >> "$LOG" 2>/dev/null
            udc_soft_reset
            wait_usb0_addr_ok 20 || true
            if enable_tether; then
                fail_streak=0
                unhealthy_streak=0
                mark_repair
                log_msg "tether recovered after usb0 absent (round $n)"
            else
                fail_streak=$((fail_streak + 1))
                mark_repair
                log_msg "usb0 absent repair failed streak=$fail_streak (round $n)"
            fi
        fi
    fi
    if [ "$n" -le 45 ]; then
        sleep 2
    else
        sleep 10
    fi
done

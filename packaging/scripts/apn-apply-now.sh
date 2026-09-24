#!/bin/sh
# Apply APN by writing ofono persist then bouncing cellular.
# Proven on-device: soft Active=false often fails (InUse / autoconnect);
# persist + disable/enable cellular reliably reloads APN.
#
# Usage: apn-apply-now.sh <apn> [auth] [user] [pass] [proto]
#        apn-apply-now.sh --clear

CTX=/ril_0/context1
DBG=/tmp/apn-apply-debug.ndjson
APN="$1"
AUTH="${2:-none}"
USER="${3:-}"
PASS="${4:-}"
PROTO="${5:-dual}"

logj() {
  hyp="$1"; msg="$2"; data="$3"
  echo "{\"sessionId\":\"0c2eee\",\"hypothesisId\":\"$hyp\",\"location\":\"apn-apply-now.sh\",\"message\":\"$msg\",\"data\":$data,\"timestamp\":$(date +%s)000}" >> "$DBG" 2>/dev/null
}

is_active() {
  dbus-send --system --print-reply --dest=org.ofono "$CTX" \
    org.ofono.ConnectionContext.GetProperties 2>/dev/null | \
    sed -n '/string "Active"/,+1p' | tail -1 | grep -q 'boolean true'
}

get_apn() {
  dbus-send --system --print-reply --dest=org.ofono "$CTX" \
    org.ofono.ConnectionContext.GetProperties 2>/dev/null | \
    sed -n '/AccessPointName/,+1p' | tail -1 | \
    sed 's/.*string "\(.*\)".*/\1/'
}

context_exists() {
  dbus-send --system --print-reply --dest=org.ofono "$CTX" \
    org.ofono.ConnectionContext.GetProperties >/dev/null 2>&1
}

find_imsi() {
  imsi=$(find /mnt/data/ofono -maxdepth 1 -type d 2>/dev/null | sed 's|.*/||' | \
    grep -E '^[0-9]{15}$' | sed -n '1p')
  if [ -n "$imsi" ]; then
    echo "$imsi"
    return 0
  fi
  connmanctl services 2>/dev/null | grep -oE '460[0-9]{12}' | sed -n '1p'
}

find_svc() {
  connmanctl services 2>/dev/null | grep -oE 'cellular_[^[:space:]]+_context1' | sed -n '1p'
}

write_persist() {
  apn="$1"
  auth="$2"
  user="$3"
  pass="$4"
  proto="$5"
  imsi=$(find_imsi)
  [ -n "$imsi" ] || return 1
  dir=/mnt/data/ofono/$imsi
  mkdir -p "$dir"
  if [ -z "$apn" ]; then
    rm -f "$dir/defult_apn"
    if [ -f "$dir/gprs" ]; then
      sed -i 's/^AccessPointName=.*/AccessPointName=/' "$dir/gprs"
    fi
  else
    cat > "$dir/defult_apn" <<EOF
[userDefultApn]
AccessPointName=$apn
Username=$user
Password=$pass
AuthenticationMethod=$auth
Protocol=$proto
EOF
    if [ -f "$dir/gprs" ]; then
      sed -i "s/^AccessPointName=.*/AccessPointName=$apn/" "$dir/gprs"
      sed -i "s/^AuthenticationMethod=.*/AuthenticationMethod=$auth/" "$dir/gprs"
      sed -i "s/^Username=.*/Username=$user/" "$dir/gprs"
      sed -i "s/^Password=.*/Password=$pass/" "$dir/gprs"
    fi
  fi
  sync
  return 0
}

bounce_cellular() {
  svc=$(find_svc)
  [ -n "$svc" ] && connmanctl config "$svc" --autoconnect no >/dev/null 2>&1
  [ -n "$svc" ] && connmanctl disconnect "$svc" >/dev/null 2>&1
  dbus-send --system --dest=org.ofono "$CTX" \
    org.ofono.ConnectionContext.SetProperty string:Active variant:boolean:false >/dev/null 2>&1
  n=0
  while [ "$n" -lt 10 ]; do
    if ! context_exists; then
      break
    fi
    if ! is_active; then
      break
    fi
    sleep 1
    n=$((n + 1))
    [ -n "$svc" ] && connmanctl disconnect "$svc" >/dev/null 2>&1
    dbus-send --system --dest=org.ofono "$CTX" \
      org.ofono.ConnectionContext.SetProperty string:Active variant:boolean:false >/dev/null 2>&1
  done
  if context_exists && is_active; then
    connmanctl disable cellular >/dev/null 2>&1
    sleep 3
  fi
  # If context still exists and inactive, push APN via dbus as well
  if [ -n "$APN" ] && [ "$APN" != "--clear" ] && context_exists && ! is_active; then
    dbus-send --system --dest=org.ofono "$CTX" \
      org.ofono.ConnectionContext.SetProperty string:AccessPointName variant:string:"$APN" >/dev/null 2>&1
    [ -n "$AUTH" ] && dbus-send --system --dest=org.ofono "$CTX" \
      org.ofono.ConnectionContext.SetProperty string:AuthenticationMethod variant:string:"$AUTH" >/dev/null 2>&1
  fi
  connmanctl enable cellular >/dev/null 2>&1
  sleep 2
  svc=$(find_svc)
  [ -n "$svc" ] && connmanctl config "$svc" --autoconnect yes >/dev/null 2>&1
  [ -n "$svc" ] && connmanctl connect "$svc" >/dev/null 2>&1
  connmanctl ActivatePdp 1 >/dev/null 2>&1
  sleep 6
  if context_exists && ! is_active; then
    dbus-send --system --dest=org.ofono "$CTX" \
      org.ofono.ConnectionContext.SetProperty string:Active variant:boolean:true >/dev/null 2>&1
    sleep 4
  fi
}

: >> "$DBG"
before=$(get_apn)

if [ "$APN" = "--clear" ] || [ -z "$APN" ]; then
  logj H_algo "clear_start" "{\"before\":\"$before\"}"
  write_persist "" none "" "" dual || exit 1
  APN=""
  bounce_cellular
  after=$(get_apn)
  logj H_algo "clear_done" "{\"after\":\"$after\",\"active\":$(is_active && echo 1 || echo 0)}"
  echo "after=$after"
  exit 0
fi

logj H_algo "apply_start" "{\"before\":\"$before\",\"target\":\"$APN\"}"
write_persist "$APN" "$AUTH" "$USER" "$PASS" "$PROTO" || {
  logj H_persist "persist_fail" "{\"imsi\":\"$(find_imsi)\"}"
  exit 1
}
bounce_cellular
after=$(get_apn)
active=$(is_active && echo 1 || echo 0)
echo "after=$after active=$active"
if [ "$after" = "$APN" ]; then
  logj H_algo "SUCCESS" "{\"after\":\"$after\",\"active\":$active}"
  exit 0
fi
logj H_algo "FAIL" "{\"after\":\"$after\",\"target\":\"$APN\",\"active\":$active}"
exit 1

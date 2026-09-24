#!/bin/sh
# ofono-server launcher for /home/root/6677
# Use absolute path + nohup so ADB/loader session exit does not kill server (SIGHUP).
PID_FILE=/tmp/6677-server.pid
BIN=/home/root/6677/server
PORT=80
LOG=/tmp/6677-server.start.log
TETHER=/home/root/usb-tether.sh

cd /home/root/6677 || exit 1
chmod 755 "$BIN" 2>/dev/null || true

if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE" 2>/dev/null)" 2>/dev/null; then
    :
else
    if netstat -tln 2>/dev/null | grep -q ":${PORT} "; then
        for pid in $(ps | grep "[s]erver ${PORT}" | awk '{print $1}'); do
            kill -9 "$pid" 2>/dev/null
        done
        sleep 1
    fi

    if [ -x "$BIN" ]; then
        nohup "$BIN" "$PORT" >"$LOG" 2>&1 &
        echo $! > "$PID_FILE"
    fi
fi

# Ensure RNDIS DHCP daemon (loader may be absent/non-executable after absorb)
if [ -f "$TETHER" ]; then
    chmod 755 "$TETHER" 2>/dev/null || true
    if ! ps | grep '[u]sb-tether' >/dev/null 2>&1; then
        rm -f /tmp/usb-tether.pid 2>/dev/null
        setsid "$TETHER" >> /tmp/usb-tether.log 2>&1 &
    fi
fi

exit 0

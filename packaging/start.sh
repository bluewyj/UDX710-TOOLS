#!/bin/sh
# ofono-server launcher for /home/root/6677
# Use absolute path + nohup so ADB/loader session exit does not kill server (SIGHUP).
PID_FILE=/tmp/6677-server.pid
BIN=/home/root/6677/server
PORT=80
LOG=/tmp/6677-server.start.log

cd /home/root/6677 || exit 1
chmod 755 "$BIN" 2>/dev/null || true

if [ -f "$PID_FILE" ] && kill -0 "$(cat "$PID_FILE" 2>/dev/null)" 2>/dev/null; then
    exit 0
fi

if netstat -tln 2>/dev/null | grep -q ":${PORT} "; then
    for pid in $(ps | grep "[s]erver ${PORT}" | awk '{print $1}'); do
        kill -9 "$pid" 2>/dev/null
    done
    sleep 1
fi

[ -x "$BIN" ] || exit 0

nohup "$BIN" "$PORT" >"$LOG" 2>&1 &
echo $! > "$PID_FILE"
exit 0

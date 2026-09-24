#!/bin/sh
# Install CI artifact layout into device runtime paths.
# Expected cwd = extracted update root (contains 6677/ and optional apn-apply-now.sh).
set -e
ROOT=/home/root
mkdir -p "$ROOT/6677"
if [ -f 6677/server ]; then
  cp -f 6677/server "$ROOT/6677/server"
  chmod 755 "$ROOT/6677/server"
fi
if [ -d 6677/dist ]; then
  rm -rf "$ROOT/6677/dist"
  cp -a 6677/dist "$ROOT/6677/dist"
fi
if [ -f 6677/start.sh ]; then
  cp -f 6677/start.sh "$ROOT/6677/start.sh"
  chmod 755 "$ROOT/6677/start.sh"
fi
if [ -f apn-apply-now.sh ]; then
  cp -f apn-apply-now.sh "$ROOT/apn-apply-now.sh"
  chmod 755 "$ROOT/apn-apply-now.sh"
fi
if [ -f /tmp/6677-server.pid ]; then
  kill "$(cat /tmp/6677-server.pid)" 2>/dev/null || true
  rm -f /tmp/6677-server.pid
fi
for pid in $(ps | grep '[s]erver 80' | awk '{print $1}'); do
  kill -9 "$pid" 2>/dev/null || true
done
sleep 1
[ -x "$ROOT/6677/start.sh" ] && "$ROOT/6677/start.sh" || true
echo "install ok"

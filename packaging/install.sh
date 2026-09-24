#!/bin/sh
# Install CI artifact layout into device runtime paths.
# MUST work when invoked as: sh /tmp/update/install.sh  (cwd may NOT be /tmp/update)
set -e
SELF="$0"
case "$SELF" in
  /*) ;;
  *) SELF="$(pwd)/$SELF" ;;
esac
DIR=$(dirname "$SELF")
ROOT=/home/root
LOG=/tmp/ota-install.log
{
  echo "=== ota install start $(date) dir=$DIR cwd=$(pwd) ==="
  ls -la "$DIR" || true
  ls -la "$DIR/6677" || true

  mkdir -p "$ROOT/6677"

  if [ -f "$DIR/6677/server" ]; then
    cp -f "$DIR/6677/server" "$ROOT/6677/server"
    chmod 755 "$ROOT/6677/server"
    echo "installed server bytes=$(wc -c < "$ROOT/6677/server")"
  else
    echo "ERROR: missing $DIR/6677/server"
    exit 1
  fi

  if [ -d "$DIR/6677/dist" ]; then
    rm -rf "$ROOT/6677/dist"
    cp -a "$DIR/6677/dist" "$ROOT/6677/dist"
    echo "installed dist"
  fi

  if [ -f "$DIR/6677/start.sh" ]; then
    cp -f "$DIR/6677/start.sh" "$ROOT/6677/start.sh"
    chmod 755 "$ROOT/6677/start.sh"
  fi

  if [ -f "$DIR/apn-apply-now.sh" ]; then
    cp -f "$DIR/apn-apply-now.sh" "$ROOT/apn-apply-now.sh"
    chmod 755 "$ROOT/apn-apply-now.sh"
    echo "installed apn-apply-now.sh"
  fi

  # stop old server (do not kill adbd)
  if [ -f /tmp/6677-server.pid ]; then
    kill "$(cat /tmp/6677-server.pid)" 2>/dev/null || true
    rm -f /tmp/6677-server.pid
  fi
  for p in $(ps | grep '[/]home/root/6677/server' | awk '{print $1}'); do
    kill "$p" 2>/dev/null || true
  done
  sleep 1

  if [ -x "$ROOT/6677/start.sh" ]; then
    "$ROOT/6677/start.sh" || true
  else
    nohup "$ROOT/6677/server" 80 >/tmp/6677-server.start.log 2>&1 &
    echo $! >/tmp/6677-server.pid
  fi
  echo "install ok"
} >"$LOG" 2>&1
cat "$LOG"

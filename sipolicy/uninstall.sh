#!/system/bin/sh
# Stop the daemon.  User rules and the log are kept on purpose; remove
# /data/adb/sipolicy manually if they are no longer wanted.

DATA=/data/adb/sipolicy

if [ -f "$DATA/sipolicyd.pid" ]; then
    pid="$(cat "$DATA/sipolicyd.pid" 2>/dev/null)"
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null
        sleep 1
        kill -9 "$pid" 2>/dev/null
    fi
fi

rm -f "$DATA/sipolicy.sock" "$DATA/state" "$DATA/state.tmp" \
      "$DATA/sipolicyd.pid"

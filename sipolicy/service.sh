#!/system/bin/sh
# late_start service: bring up sipolicyd once Android has booted.

MODDIR=${0%/*}
DATA=/data/adb/sipolicy
BIN="$MODDIR/system/bin/sipolicyd"
LOG="$DATA/sipolicy.log"

mkdir -p "$DATA/rules"
chmod 0700 "$DATA" "$DATA/rules" 2>/dev/null

i=0
while [ "$(getprop sys.boot_completed)" != "1" ] && [ "$i" -lt 180 ]; do
    sleep 1
    i=$((i + 1))
done

if [ -f "$DATA/sipolicyd.pid" ]; then
    pid="$(cat "$DATA/sipolicyd.pid" 2>/dev/null)"
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        exit 0
    fi
fi

if [ ! -x "$BIN" ]; then
    echo "sipolicy: $BIN is missing or not executable" >>"$LOG"
    exit 1
fi

if command -v setsid >/dev/null 2>&1; then
    setsid "$BIN" --data "$DATA" >>"$LOG" 2>&1 &
else
    "$BIN" --data "$DATA" >>"$LOG" 2>&1 &
fi

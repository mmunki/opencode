#!/system/bin/sh
# Early boot: make the data directory and seed the default policy.  The
# daemon itself is started by service.sh after the system is up.

MODDIR=${0%/*}
DATA=/data/adb/sipolicy

mkdir -p "$DATA/rules"
chmod 0700 "$DATA" "$DATA/rules" 2>/dev/null

if [ ! -f "$DATA/sipolicy.rule" ] && [ -f "$MODDIR/rules/sipolicy.rule" ]; then
    cp -f "$MODDIR/rules/sipolicy.rule" "$DATA/sipolicy.rule"
    chmod 0600 "$DATA/sipolicy.rule"
fi

# Runtime files from a previous boot are stale.
rm -f "$DATA/sipolicy.sock" "$DATA/state" "$DATA/state.tmp"

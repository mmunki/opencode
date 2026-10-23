#!/system/bin/sh

SKIPUNZIP=0

ui_print "*******************************"
ui_print " sipolicy 0.109"
ui_print " kernel-assisted command policy"
ui_print "*******************************"

case "$ARCH" in
    arm64)
        ABI=arm64-v8a
        LIBDIR=lib64
        ;;
    arm)
        ABI=armeabi-v7a
        LIBDIR=lib
        ;;
    x86_64)
        ABI=x86_64
        LIBDIR=lib64
        ;;
    x86)
        ABI=x86
        LIBDIR=lib
        ;;
    *)
        abort "! Unsupported architecture: $ARCH"
        ;;
esac

ui_print "- device architecture: $ARCH ($ABI)"

if [ ! -d "$MODPATH/native/$ABI" ]; then
    abort "! Missing native payload for $ABI"
fi

mkdir -p "$MODPATH/system/bin" "$MODPATH/system/$LIBDIR"
cp -f "$MODPATH/native/$ABI/sipolicyd" "$MODPATH/system/bin/sipolicyd" || \
    abort "! Cannot install sipolicyd"
cp -f "$MODPATH/native/$ABI/sipolicy" "$MODPATH/system/bin/sipolicy" || \
    abort "! Cannot install sipolicy"
cp -f "$MODPATH/native/$ABI/sipolicy-run" "$MODPATH/system/bin/sipolicy-run" || \
    abort "! Cannot install sipolicy-run"
cp -f "$MODPATH/native/$ABI/libsipolicy.so" \
    "$MODPATH/system/$LIBDIR/libsipolicy.so" || \
    abort "! Cannot install libsipolicy.so"

set_perm "$MODPATH/system/bin/sipolicyd" 0 0 0755
set_perm "$MODPATH/system/bin/sipolicy" 0 0 0755
set_perm "$MODPATH/system/bin/sipolicy-run" 0 0 0755
set_perm "$MODPATH/system/$LIBDIR/libsipolicy.so" 0 0 0644
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755
set_perm "$MODPATH/uninstall.sh" 0 0 0755

# The native directory is build staging only; the installed copies live in
# system/bin and system/lib(64), so the module stays small.
rm -rf "$MODPATH/native"

mkdir -p /data/adb/sipolicy/rules
chmod 0700 /data/adb/sipolicy /data/adb/sipolicy/rules 2>/dev/null

if [ ! -f /data/adb/sipolicy/sipolicy.rule ]; then
    cp -f "$MODPATH/rules/sipolicy.rule" /data/adb/sipolicy/sipolicy.rule
    chmod 0600 /data/adb/sipolicy/sipolicy.rule
    ui_print "- installed default /data/adb/sipolicy/sipolicy.rule"
else
    ui_print "- keeping existing /data/adb/sipolicy/sipolicy.rule"
fi

ui_print "- binaries: sipolicyd, sipolicy, sipolicy-run"
ui_print "- library: system/$LIBDIR/libsipolicy.so"
ui_print "- rules: /data/adb/sipolicy/sipolicy.rule"
ui_print "- other modules may drop sipolicy.rule in their module root"
ui_print "- reboot, then run: sipolicy status"

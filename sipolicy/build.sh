#!/usr/bin/env bash
# ============================================================================
# sipolicy build -- cross-compile the native payload for the four Android
# ABIs and package the Magisk module.
#
#   bash sipolicy/build.sh
#
# Output (never committed):
#   sipolicy/dist/sipolicy-0.109.zip
#   sipolicy/dist/sipolicy-0.109.zip.sha256
# ============================================================================
set -euo pipefail

VERSION="0.109"
API=21

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SRC="$SCRIPT_DIR"
BUILD="${SIPOLICY_BUILD_DIR:-$SRC/build}"
OUT="${SIPOLICY_OUT_DIR:-$SRC/dist}"
NATIVE="$BUILD/native"
MOD="$BUILD/module"

log() { printf '==> %s\n' "$*" >&2; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

detect_ndk() {
    local candidate base version

    for candidate in "${ANDROID_NDK_HOME:-}" "${ANDROID_NDK_ROOT:-}" \
                     "${ANDROID_NDK:-}" "${NDK_HOME:-}"; do
        [ -n "$candidate" ] || continue
        if [ -x "$candidate/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    for base in "${ANDROID_HOME:-}/ndk" "${ANDROID_SDK_ROOT:-}/ndk" \
                /usr/local/lib/android/sdk/ndk /opt/android-sdk/ndk \
                /opt/android/ndk "$HOME/Android/Sdk/ndk"; do
        [ -d "$base" ] || continue
        while IFS= read -r version; do
            candidate="$base/$version"
            if [ -x "$candidate/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]; then
                printf '%s\n' "$candidate"
                return 0
            fi
        done < <(ls -1 "$base" 2>/dev/null | sort -V -r)
    done
    return 1
}

NDK="${SIPOLICY_NDK:-$(detect_ndk || true)}"
[ -n "$NDK" ] && [ -d "$NDK" ] || \
    die "Android NDK not found; set ANDROID_NDK_HOME"
TOOLCHAIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
[ -x "$TOOLCHAIN/bin/clang" ] || die "invalid NDK: $NDK"
STRIP="$TOOLCHAIN/bin/llvm-strip"

log "NDK: $NDK"
log "version: $VERSION, API level: $API"

rm -rf "$BUILD"
mkdir -p "$BUILD" "$OUT" "$NATIVE"

# ---------------------------------------------------------------------------
# Host self-tests for the rule engine.  These run before any cross-compile so
# a broken parser stops the release.
# ---------------------------------------------------------------------------
HOSTCC="${HOSTCC:-cc}"
command -v "$HOSTCC" >/dev/null 2>&1 || die "host C compiler not found"
log "building host self-tests"
"$HOSTCC" -O2 -std=c11 -Wall -Wextra -Wno-unused-parameter \
    -I"$SRC/src" \
    "$SRC/src/selftest.c" "$SRC/src/rules.c" "$SRC/src/util.c" \
    -o "$BUILD/selftest"
"$BUILD/selftest" || die "host self-tests failed"

# ---------------------------------------------------------------------------
# Cross-compile.
# ---------------------------------------------------------------------------
CFLAGS=(-O2 -std=c11 -D_FILE_OFFSET_BITS=64
        -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing)
LDFLAGS=(-Wl,-z,relro,-z,now -Wl,--build-id=sha1)

build_one() {
    local abi="$1" triple="$2"
    local cc="$TOOLCHAIN/bin/${triple}${API}-clang"
    local dir="$NATIVE/$abi"

    [ -x "$cc" ] || die "compiler not found: $cc"
    mkdir -p "$dir"
    log "building $abi"

    "$cc" "${CFLAGS[@]}" -fPIE -pie "${LDFLAGS[@]}" -I"$SRC/src" \
        "$SRC/src/sipolicyd.c" "$SRC/src/rules.c" "$SRC/src/util.c" \
        -o "$dir/sipolicyd"
    "$cc" "${CFLAGS[@]}" -fPIE -pie "${LDFLAGS[@]}" -I"$SRC/src" \
        "$SRC/src/sipolicy.c" "$SRC/src/util.c" \
        -o "$dir/sipolicy"
    "$cc" "${CFLAGS[@]}" -fPIE -pie "${LDFLAGS[@]}" -I"$SRC/src" \
        "$SRC/src/sipolicy-run.c" "$SRC/src/util.c" \
        -o "$dir/sipolicy-run"
    "$cc" "${CFLAGS[@]}" -fPIC -shared "${LDFLAGS[@]}" \
        -Wl,-soname,libsipolicy.so -I"$SRC/src" \
        "$SRC/src/libsipolicy.c" "$SRC/src/util.c" \
        -ldl -o "$dir/libsipolicy.so"

    "$STRIP" --strip-unneeded "$dir/sipolicyd" "$dir/sipolicy" \
        "$dir/sipolicy-run" "$dir/libsipolicy.so"

    for binary in sipolicyd sipolicy sipolicy-run libsipolicy.so; do
        [ -s "$dir/$binary" ] || die "missing build output: $dir/$binary"
    done
}

build_one arm64-v8a aarch64-linux-android
build_one armeabi-v7a armv7a-linux-androideabi
build_one x86_64 x86_64-linux-android
build_one x86 i686-linux-android

# ---------------------------------------------------------------------------
# Stage the Magisk module.  module.prop and META-INF must be at the zip root.
# ---------------------------------------------------------------------------
log "staging module"
rm -rf "$MOD"
mkdir -p "$MOD/META-INF/com/google/android" "$MOD/rules"

cp "$SRC/module.prop" "$MOD/module.prop"
cp "$SRC/customize.sh" "$MOD/customize.sh"
cp "$SRC/post-fs-data.sh" "$MOD/post-fs-data.sh"
cp "$SRC/service.sh" "$MOD/service.sh"
cp "$SRC/uninstall.sh" "$MOD/uninstall.sh"
cp "$SRC/README.md" "$MOD/README.md"
cp "$SRC/CHANGELOG.md" "$MOD/CHANGELOG.md"
cp "$SRC/rules/sipolicy.rule" "$MOD/rules/sipolicy.rule"
cp "$SRC/META-INF/com/google/android/update-binary" \
    "$MOD/META-INF/com/google/android/update-binary"
cp "$SRC/META-INF/com/google/android/updater-script" \
    "$MOD/META-INF/com/google/android/updater-script"

for abi in arm64-v8a armeabi-v7a x86_64 x86; do
    mkdir -p "$MOD/native/$abi"
    cp "$NATIVE/$abi/sipolicyd" "$MOD/native/$abi/sipolicyd"
    cp "$NATIVE/$abi/sipolicy" "$MOD/native/$abi/sipolicy"
    cp "$NATIVE/$abi/sipolicy-run" "$MOD/native/$abi/sipolicy-run"
    cp "$NATIVE/$abi/libsipolicy.so" "$MOD/native/$abi/libsipolicy.so"
done

chmod 0755 "$MOD/customize.sh" "$MOD/post-fs-data.sh" "$MOD/service.sh" \
    "$MOD/uninstall.sh" "$MOD/META-INF/com/google/android/update-binary"
chmod 0644 "$MOD/module.prop" "$MOD/README.md" "$MOD/CHANGELOG.md" \
    "$MOD/rules/sipolicy.rule" "$MOD/META-INF/com/google/android/updater-script"
find "$MOD/native" -type f -name 'libsipolicy.so' -exec chmod 0644 {} +
find "$MOD/native" -type f ! -name 'libsipolicy.so' -exec chmod 0755 {} +

# ---------------------------------------------------------------------------
# Package.  The zip contains module.prop at its root, as Magisk expects.
# ---------------------------------------------------------------------------
ZIP_NAME="sipolicy-$VERSION.zip"
ZIP_PATH="$OUT/$ZIP_NAME"
SUM_PATH="$OUT/$ZIP_NAME.sha256"

rm -f "$ZIP_PATH" "$SUM_PATH"
log "packaging $ZIP_NAME"
command -v zip >/dev/null 2>&1 || die "zip is required"
(cd "$MOD" && zip -X -r -9 "$ZIP_PATH" . >/dev/null)

log "verifying the archive"
unzip -t "$ZIP_PATH" >/dev/null || die "archive test failed"
unzip -l "$ZIP_PATH" | grep -q ' module.prop$' || \
    die "module.prop is not at the zip root"
for abi in arm64-v8a armeabi-v7a x86_64 x86; do
    unzip -l "$ZIP_PATH" | grep -q " native/$abi/libsipolicy.so$" || \
        die "zip is missing the $abi library"
done
[ -f "$MOD/META-INF/com/google/android/update-binary" ] || \
    die "missing update-binary"

(cd "$OUT" && sha256sum "$ZIP_NAME" > "$ZIP_NAME.sha256")
[ -s "$SUM_PATH" ] || die "checksum was not written"

if command -v file >/dev/null 2>&1; then
    for abi in arm64-v8a armeabi-v7a x86_64 x86; do
        file "$NATIVE/$abi/sipolicyd" | grep -q 'ELF' || \
            die "$abi/sipolicyd is not an ELF binary"
    done
fi

log "built: $ZIP_PATH"
log "checksum: $(cut -d' ' -f1 "$SUM_PATH")"
printf '\n%s\n' "$ZIP_PATH"

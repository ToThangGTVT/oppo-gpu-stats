#!/usr/bin/env bash
# Build daemon statd bằng NDK, đẩy lên máy qua adb và chạy nền với quyền shell.
#   ./tools/statd.sh             # build theo ABI của máy đang cắm, push + chạy
#   ./tools/statd.sh --root      # chạy bằng su (máy đã root, đọc được tải GPU)
#   ./tools/statd.sh --build-only arm64-v8a
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/app/src/main/cpp"
NDK_VERSION=28.2.13676358
API=26

mode=run
abi=""
for arg in "$@"; do
    case "$arg" in
        --root) mode=root ;;
        --build-only) mode=build ;;
        *) abi="$arg" ;;
    esac
done

sdk="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Library/Android/sdk}}"
ndk="${ANDROID_NDK_HOME:-$sdk/ndk/$NDK_VERSION}"
case "$(uname -s)" in
    Darwin) host=darwin-x86_64 ;;
    *) host=linux-x86_64 ;;
esac
bin="$ndk/toolchains/llvm/prebuilt/$host/bin"
[ -d "$bin" ] || { echo "Không tìm thấy NDK tại $ndk (đặt ANDROID_NDK_HOME)"; exit 1; }

[ -n "$abi" ] || abi="$(adb shell getprop ro.product.cpu.abi | tr -d '\r')"
case "$abi" in
    arm64-v8a) triple=aarch64-linux-android ;;
    armeabi-v7a) triple=armv7a-linux-androideabi ;;
    x86_64) triple=x86_64-linux-android ;;
    *) echo "ABI không hỗ trợ: $abi"; exit 1 ;;
esac

out="$ROOT/build/statd/$abi"
mkdir -p "$out"
"$bin/${triple}${API}-clang++" -std=c++17 -O2 -Wall -static-libstdc++ \
    "$SRC/statd.cpp" "$SRC/stats.cpp" -o "$out/statd"
echo "Đã build $out/statd"

[ "$mode" = build ] && exit 0

adb push "$out/statd" /data/local/tmp/statd >/dev/null
adb shell chmod 755 /data/local/tmp/statd
adb shell 'pkill -f /data/local/tmp/statd' >/dev/null 2>&1 || true
if [ "$mode" = root ]; then
    adb shell su -c /data/local/tmp/statd
else
    adb shell /data/local/tmp/statd
fi

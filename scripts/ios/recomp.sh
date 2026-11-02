#!/usr/bin/env bash
# iOS proof of concept (branch ios-poc): the recompiled library for iPhones, from the C that
# recomp/tools/recomp/recomp.sh generated from the player's own eboot.bin (recomp/out/recomp/gen).
# Same flags as the macOS library; the result, out/ios/librecomp.dylib, stays on this Mac and in
# the player's own app build. It is never committed.
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
gen=${BB_RECOMP_GEN:-recomp/out/recomp/gen} # BB_RECOMP_GEN: another generated tree
out=out/ios/recomp
[[ -n $(find "$gen" -maxdepth 1 -name 'recomp*.c' -print -quit) ]] ||
    { echo "No generated C in $gen: run recomp/tools/recomp/recomp.sh first" >&2; exit 1; }
mkdir -p "$out/$gen" # objects at $out/<source path>.o
clang=(xcrun -sdk iphoneos clang -target arm64-apple-ios18.0)
# Only files newer than their object are compiled again. Floating point as on x86: no fused
# multiply-add.
changed=()
for f in "$gen"/recomp*.c; do
    o=$out/$f.o
    [[ -e $o && $o -nt $f ]] || changed+=("$f")
done
echo "${#changed[@]} files to compile"
printf '%s\n' "${changed[@]}" | grep . | xargs -P "$(sysctl -n hw.ncpu)" -I{} "${clang[@]}" -O2 -g -std=gnu11 \
    -w -ffp-contract=off -fno-strict-aliasing -fvisibility=hidden -Ideps/ios-arm64/include -Ibbcpu/src \
    -c {} -o "$out/{}.o" || true
for f in "${changed[@]}"; do [[ -e $out/$f.o ]] || { echo "failed: $f"; exit 1; }; done
"${clang[@]}" -dynamiclib -install_name @rpath/librecomp.dylib "$out"/"$gen"/recomp*.o -o out/ios/librecomp.dylib
echo out/ios/librecomp.dylib

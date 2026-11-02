#!/usr/bin/env bash
# iOS proof of concept (branch ios-poc): the runtime (loader, system libraries, bbcpu, LibAtrac9)
# for arm64 iPhones as out/ios/libbbios.a; BB_IOS_GPU=0 adds ios/bbgpu_stub.c in place of the GPU
# library (milestone P1, --cpu-only). bb-probe's main becomes bb_probe_main, called by ios/app_main.c.
# Needs scripts/ios/build-deps.sh first.
set -euo pipefail
cd "$(dirname "$0")/../.."
export DEVELOPER_DIR=${DEVELOPER_DIR:-/Applications/Xcode.app/Contents/Developer}
cc=(xcrun -sdk iphoneos clang -target arm64-apple-ios18.0)
deps=deps/ios-arm64
obj=out/ios/core
rm -rf "$obj" && mkdir -p "$obj"
compile() { # compile SOURCE FLAGS...: one object in $obj
    local source=$1; shift
    "${cc[@]}" "$@" -c "$source" -o "$obj/$(basename "$source").o"
}
stub=(); [[ ${BB_IOS_GPU:-1} == 0 ]] && stub=(ios/bbgpu_stub.c)
for source in src/probe.c src/runtime*.c src/platform.c src/vulkan_smoke.c "${stub[@]}"; do
    compile "$source" -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-unknown-warning-option \
        -Wno-unused-but-set-global -pthread -Dmain=bb_probe_main -I"$deps/include" -I. -Isrc -Ibbcpu/src
done
for source in bbcpu/src/cpu/*.c bbcpu/src/cpu/hostcall.S; do
    compile "$source" -std=gnu11 -O2 -g -Wall -Wextra -Werror -Wno-unused-parameter -I"$deps/include"
done
for source in third_party/LibAtrac9/C/src/*.c; do compile "$source" -std=c99 -O2 -g -w; done
rm -f out/ios/libbbios.a
libtool -static -o out/ios/libbbios.a "$obj"/*.o "$deps/lib/libZydis.a" "$deps/lib/libZycore.a" 2>/dev/null
echo out/ios/libbbios.a

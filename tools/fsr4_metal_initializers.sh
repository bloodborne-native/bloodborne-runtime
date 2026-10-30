#!/usr/bin/env bash
# tools/fsr4_metal_initializers.sh <amd_dll> [OUT]
#
# FSR 4.1.x on Metal (gpu/fsr4_metal) needs AMD's model initializers from the player's own DLL
# (nothing of AMD's is downloaded or shipped). This script recognizes the DLLs it knows and
# extracts the files at their fixed offsets, checking the DLL and every file by SHA-256:
#
#  - FSR 4.1 / FSR 4.1 Lossy: initializers.bin and initializers-ultraperformance.bin, 131,072
#    bytes each, from the FSR 4.1.1.2740 upscaler DLL
#    (amd_fidelityfx_upscaler_dx12.dll of FidelityFX SDK 2.3.0);
#  - FSR 4.1.2 Lite: initializers-lite.bin and initializers-lite-ultraperformance.bin, 27,392
#    bytes each, from amdvkffx64.dll or amdxcffx64.dll (AMD's 4.1.2 driver).
#
# OUT defaults to $BB_DATA_DIR/fsr4_411 or fsr4_412 (else fsr4_411/4_412 in this tree): the
# folders run.sh passes to the game as BB_FSR411_DIR / BB_FSR412_DIR. Needs dd and shasum.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
dll=${1:?usage: tools/fsr4_metal_initializers.sh <amd_dll> [OUT]}
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }
dll_sha=$(sha "$dll")
block=512
size=131072
version=4.1.1
out=${2:-${BB_DATA_DIR:-.}/fsr4_411}

case "$dll_sha" in
d0dcccc74a43c44ba435b7a369b456e0970d8a4464e4bd683119b374f2c9fb46)
    # The 4.1.1.2740 upscaler DLL (FidelityFX SDK 2.3.0).
    ;;
80e761f7c8c0ec8ead7b321df01911baaf917defae51388d29eaecb1d6bc58bd)
    # AMD's 4.1.2 Vulkan provider (amdvkffx64.dll).
    block=16; size=27392; version=4.1.2
    out=${2:-${BB_DATA_DIR:-.}/fsr4_412}
    ;;
c33a287f3048bf02311cae430b6a977500b94ca7b315cd7bff751e6b03e483e3)
    # AMD's 4.1.2 DX12 provider (amdxcffx64.dll).
    block=16; size=27392; version=4.1.2
    out=${2:-${BB_DATA_DIR:-.}/fsr4_412}
    ;;
*)
    echo "Not a known AMD FSR 4.1.x DLL (SHA-256 $dll_sha)." >&2
    echo "Expected amd_fidelityfx_upscaler_dx12.dll (4.1.1.2740), amdvkffx64.dll or amdxcffx64.dll (4.1.2)." >&2
    exit 2
    ;;
esac

mkdir -p "$out"
extract() { # name offset expected-sha
    local name=$1 offset=$2 expected=$3
    dd if="$dll" of="$out/$name.part" bs=$block skip=$((offset / block)) \
       count=$((size / block)) status=none
    if [[ $(sha "$out/$name.part") != "$expected" ]]; then
        rm -f -- "$out/$name.part"
        echo "$name: unexpected bytes in the DLL" >&2
        exit 3
    fi
    mv "$out/$name.part" "$out/$name"
}

if [[ $version == 4.1.1 ]]; then
    extract initializers.bin 12601856 \
        a54e552ff69a3f7199861417f7ae461d1f67f636ab532c6ec9d8ea5d2963735a
    extract initializers-ultraperformance.bin 12732928 \
        3c42fc6eabfa9b5b1e489df0e9b2f58e3f9fa22048c584f6839bb27d32a0640e
else
    if [[ $dll_sha == 80e761f7* ]]; then
        common=0x34CE3F0
        ultra=0x34E9020
    else
        common=0x3C12390
        ultra=0x3C2CFC0
    fi
    extract initializers-lite.bin $((common)) \
        4b9898da719dc2dca5dc45fd71dec5f80bdb9886a580fe3038af59efe637d55e
    extract initializers-lite-ultraperformance.bin $((ultra)) \
        4c57292d9963e82dec86134060eef0c2b66214cd1a31a9e9bc73739b5fe2d1d7
fi
echo "FSR $version initializers in $out"

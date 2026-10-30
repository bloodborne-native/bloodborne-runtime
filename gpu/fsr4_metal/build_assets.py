#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Builds what the Metal FSR 4.1.x path loads (vk_fsr4_metal.mm), at build time, from
fsr4-native-macos v2.2 (VENDOR.txt; scripts/macos/build-deps.sh puts its tree, DXC and the
SPIRV-Cross and SPIRV-Tools commands in deps/):

- fsr411_helpers.metallib, fsr411_simd.metallib (M1-M4) and fsr411_mpp_<tier>.metallib
  (M5, when this Xcode has Metal 4);
- <variant>/bindings.json and its metallibs: the PRE/POST/autoexposure/RCAS graph of each
  variant the game uses (compile_fsr411.py), for FSR 4.1 (exact-fast) and, when asked,
  FSR 4.1 Lossy (lossy2, whose six trained layers come from the pinned GPL file);
- fsr411_native_<tier>[_ultraperformance][_lossy2]/: the 4.1 native networks' manifest,
  parameters and weight recipe;
- fsr412_<variant>/ and fsr412_native_<tier>[_ultraperformance]/ when --fsr412-model points
  at the model imported from the builder's own AMD DLL (tools/fsr412_import_model.py).

AMD's initializers are never read here: a network's weights are bytes of the player's
initializer and the recipe says which (or a constant from the MIT HLSL import, or the
fetched GPL lossy2 weights) for each weight byte. Folding and the simd Winograd weights are
computed from those values, so the runtime does them after assembling (fsr411_runtime.h,
assemble_weights), on the player's Mac, after checking the initializer's SHA-256.

--verify DIR (AMD's 4.1.1 initializers in DIR) and --verify-412 MODEL (the imported 4.1.2
model) check that every recipe, folding and Winograd choice gives the upstream generators'
exact weights and parameters.
"""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import types

INIT_BYTES = 131072
LITE_INIT_BYTES = 27392
GATHER = 1 << 31
# The variants the game asks for: depth 0 at the near plane, motion vectors at the render
# size and not jittered, linear colour, optimize 1 (vk_fsr4_metal.mm).
VARIANT = dict(depth_inverted=0, low_res_mv=1, jittered_mv=0, colorspace=0, optimize=1)
# The 4.1 profiles: exact-fast is the default (MIT HLSL only); lossy2 needs the pinned GPL
# weights file. The retired 4.1.1 lossy profile is no longer built.
PROFILE_SUFFIX = {"exact-fast": "_fast", "lossy2": "_lossy2"}
PROFILE_NETWORK = {"exact-fast": "exact", "lossy2": "lossy2"}
NATIVE_SUFFIX = {"exact-fast": "", "lossy2": "_lossy2"}


def fail(message):
    sys.exit("FSR 4.1.x assets: " + message)


def run(command, cwd=None):
    result = subprocess.run([str(c) for c in command], cwd=cwd, capture_output=True, text=True)
    if result.returncode:
        fail(f"{' '.join(map(str, command))} failed:\n{result.stdout}{result.stderr}")
    return result.stdout


def variant_key(tier, ae, up, profile):
    v = VARIANT
    return (f"fsr411_{tier}_di{v['depth_inverted']}_lm{v['low_res_mv']}_jm{v['jittered_mv']}"
            f"_cs{v['colorspace']}_ae{ae}_o{v['optimize']}" + ("_up1" if up else "")
            + PROFILE_SUFFIX[profile] + "_pc1_ca1")


def native_dir(tier, up, profile):
    return f"fsr411_native_{tier}" + ("_ultraperformance" if up else "") + NATIVE_SUFFIX[profile]


def lite_variant_key(tier, ae, up):
    v = VARIANT
    # The runtime's session key always spells the preset (_up0 included).
    return (f"fsr412_{tier}_di{v['depth_inverted']}_lm{v['low_res_mv']}_jm{v['jittered_mv']}"
            f"_cs{v['colorspace']}_ae{ae}_o{v['optimize']}_up{up}_pc1_v2")


def lite_native_dir(tier, up):
    return f"fsr412_native_{tier}" + ("_ultraperformance" if up else "")


def metal_has_v4():
    probe = subprocess.run(["xcrun", "-sdk", "macosx", "metal", "-std=metal4.0", "-x", "metal",
                            "-c", "-", "-o", os.devnull], input="kernel void k(){}\n",
                           capture_output=True, text=True)
    return probe.returncode == 0


def build_kernels(here, out, tiers):
    air = out / ".air"
    air.mkdir(parents=True, exist_ok=True)

    def metallib(source, name, std, *defines):
        target = air / (name + ".air")
        run(["xcrun", "-sdk", "macosx", "metal", "-std=" + std, "-I", here, *defines, "-c",
             here / source, "-o", target])
        run(["xcrun", "-sdk", "macosx", "metallib", target, "-o", out / (name + ".metallib")])

    metallib("fsr411_helpers.metal", "fsr411_helpers", "macos-metal2.3")
    metallib("fsr411_simd.metal", "fsr411_simd", "metal3.0", "-DFSR411_SPECIALIZE=1")
    if metal_has_v4():
        for tier in tiers:
            metallib("fsr411_mpp.metal", f"fsr411_mpp_{tier}", "metal4.0", "-DFSR411_SPECIALIZE=1",
                     f"-DFSR411_MAXW={tier * 8 // 9}", f"-DFSR411_MAXH={tier // 2}")
    else:
        print("FSR 4.1.x assets: this Xcode has no Metal 4; M5 Macs use the simd kernels")
    shutil.rmtree(air)


def build_bindings(src, out, work, tiers, profiles, lossy2_weights):
    """compile_fsr411.py builds in `work` (its cache: unchanged inputs are not rebuilt); `out`
    gets what the runtime loads: bindings.json, the metallibs and the literal tables."""
    for profile in profiles:
        for tier in tiers:
            for ae in (0, 1):
                for up in (0, 1):
                    v = VARIANT
                    key = variant_key(tier, ae, up, profile)
                    command = [sys.executable, src / "tools/compile_fsr411.py", "--root", src,
                               "--builddir", work, "--res", tier,
                               "--depth-inverted", v["depth_inverted"], "--low-res-mv",
                               v["low_res_mv"], "--jittered-mv", v["jittered_mv"], "--colorspace",
                               v["colorspace"], "--auto-exposure", ae, "--optimize", v["optimize"],
                               "--ultra-performance", up, "--profile", profile,
                               "--external-post-cnb", 1, "--strict-math", 0,
                               "--post-cnb-alias", 1]
                    if profile == "lossy2":
                        command += ["--lossy2-weights", lossy2_weights]
                    run(command)
                    bindings = work / key / "bindings.json"
                    if not bindings.is_file():
                        fail(f"no bindings for {key}")
                    kernels = json.loads(bindings.read_text())["kernels"].values()
                    files = {"bindings.json", *(k["library"] for k in kernels),
                             *(k["literal_tables"] for k in kernels if k.get("literal_tables"))}
                    target = out / key
                    if target.exists():
                        shutil.rmtree(target)
                    target.mkdir(parents=True)
                    for name in files:
                        shutil.copy2(work / key / name, target / name)


# Range checks on values computed from the initializer (and its hash check): AMD's initializer
# passes them (checked again by --verify, and the runtime accepts only that initializer), the
# synthetic ones need not.
VALUE_CHECKS = ("POST accumulator exceeds", "float tail cannot be replaced", "integer requant may overflow",
                "downsample sum exceeds", "FNB/CT tail exceeds", "FNB integer accumulator",
                "INT8 dot products exceed", "expected the verified 131072-byte")
# The simd Winograd weights are kept for every pass the generator can transform: their exactness
# bounds depend on the values, so the runtime decides (assemble_weights).
WINOGRAD_BOUNDS = ("        if max(abs(v) for v in values) > 2048:\n            return None\n",
                   "        if worst >= 1 << 24:\n            return None\n")


def load_generator(src, synthetic, force_winograd=False):
    sys.path.insert(0, str(src / "tools"))
    path = src / "tools/gen_fsr411_native.py"
    text = path.read_text()
    if synthetic:
        for check in VALUE_CHECKS:
            text, count = re.subn(r'raise ValueError\("' + re.escape(check) + r'[^\n]*', "pass", text)
            if count != 1:
                fail(f"the 4.1 generator changed (check '{check}')")
    if synthetic or force_winograd:
        for bound in WINOGRAD_BOUNDS:
            if text.count(bound) != 1:
                fail("the 4.1 generator changed (Winograd bounds)")
            text = text.replace(bound, "")
    gen = types.ModuleType("bb_gen_fsr411_native" + ("_synthetic" if synthetic else "")
                           + ("_winograd" if force_winograd else ""))
    gen.__file__ = str(path)
    exec(compile(text, str(path), "exec"), gen.__dict__)
    return gen


def encode(gen, src, tier, up, initializer, profile, lossy2_weights=None):
    """The upstream 4.1 generator's output for this initializer:
    (manifest, weights, params)."""
    with tempfile.TemporaryDirectory() as temp:
        temp = Path(temp)
        init_path = temp / "initializer.bin"
        init_path.write_bytes(initializer)
        shaders = src / "third_party/fsr411/shaders"
        gen.encode_model(shaders, init_path, tier, temp / "out", "ultraperformance" if up else "normal",
                         shaders.parent / "ultraperformance", PROFILE_NETWORK[profile],
                         str(lossy2_weights) if (profile == "lossy2" and not up and lossy2_weights) else None)
        out = temp / "out"
        return (json.loads((out / "fsr411_native_manifest.json").read_text()),
                (out / "fsr411_native_weights.bin").read_bytes(),
                (out / "fsr411_native_params.bin").read_bytes())


def winograd_regions(manifest):
    """(entry, first byte, end) of each pass's simd Winograd weights (int16 values)."""
    for entry in [*manifest["passes"].values(), *manifest.get("auxiliary_passes", {}).values()]:
        w = entry.get("simd", {}).get("winograd")
        if w:
            yield entry, w["weights0"], w["weights0"] + 2 * w["weights0_count"]


def build_native(gen, src, out, tier, up, profile, lossy2_weights):
    """Runs the generator on synthetic initializers: bytes that follow the initializer are
    gathered from it, the rest are constants (including lossy2's trained weights). Five runs
    spell each gathered offset in nibbles. The Winograd regions are left to the runtime
    (computed from the folded weights)."""
    runs = {name: encode(gen, src, tier, up, data, profile, lossy2_weights) for name, data in (
        ("a", bytes([3]) * INIT_BYTES), ("b", bytes([9]) * INIT_BYTES),
        *((f"n{k}", bytes((i >> (4 * k)) & 15 for i in range(INIT_BYTES))) for k in range(5)))}
    manifest, weights_a, params = runs["a"]
    weights_b = runs["b"][1]
    for name, (other_manifest, weights, other) in runs.items():
        if other != params or len(weights) != len(weights_a) or \
                [r[1:] for r in winograd_regions(other_manifest)] != [r[1:] for r in winograd_regions(manifest)]:
            fail(f"the native layout depends on the initializer ({name})")
    computed = set()
    for _, first, end in winograd_regions(manifest):
        computed.update(range(first, end))
    recipe = []
    for i, (a, b) in enumerate(zip(weights_a, weights_b)):
        if i in computed or a == b:
            recipe.append(0 if i in computed else a)
            continue
        if (a, b) != (3, 9):
            fail(f"weight byte {i} is not a copy of an initializer byte")
        offset = sum(runs[f"n{k}"][1][i] << (4 * k) for k in range(5))
        if offset >= INIT_BYTES:
            fail(f"weight byte {i} reads past the initializer")
        recipe.append(GATHER | offset)
    # Only what the runtime reads, nothing computed from the synthetic values.
    for entry in [*manifest["passes"].values(), *manifest.get("auxiliary_passes", {}).values()]:
        entry.pop("accumulator_bounds", None)
        entry.get("simd", {}).pop("float_accumulator_bound", None)
        entry.get("simd", {}).get("winograd", {}).pop("float_accumulator_bound", None)
        folding = entry.get("lossy_transform", {}).get("conv3x3_folding")
        if folding:
            entry["lossy_transform"]["conv3x3_folding"] = {
                "drop_words_per_channel": folding["drop_words_per_channel"]}
    manifest.pop("weights_sha256", None)
    manifest["weights_recipe"] = "fsr411_native_recipe.bin"
    target = out / native_dir(tier, up, profile)
    target.mkdir(parents=True, exist_ok=True)
    (target / "fsr411_native_params.bin").write_bytes(params)
    (target / "fsr411_native_recipe.bin").write_bytes(struct.pack(f"<{len(recipe)}I", *recipe))
    (target / "fsr411_native_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest, recipe


def winograd(conv):
    """The runtime's Winograd (assemble_weights): the generator's transform and bounds."""
    g2 = ((2, 0, 0), (1, 1, 1), (1, -1, 1), (0, 0, 2))
    at = ((1, 1, 1, 0), (0, 1, -1, -1))
    values = [sum(g2[xi][ky] * g2[nu][kx] * conv[(ky * 3 + kx) * 256 + ic * 16 + oc]
                  for ky in range(3) for kx in range(3))
              for xi in range(4) for nu in range(4) for ic in range(16) for oc in range(16)]
    if max(abs(v) for v in values) > 2048:
        return None
    worst = 0
    for oc in range(16):
        column = [512 * sum(abs(values[(p * 16 + ic) * 16 + oc]) for ic in range(16)) for p in range(16)]
        for i in range(2):
            for j in range(2):
                worst = max(worst, sum(column[xi * 4 + nu] for xi in range(4) for nu in range(4)
                                       if at[i][xi] and at[j][nu]))
    return None if worst >= 1 << 24 else values


def assemble(src, out, tier, up, initializer, profile):
    """What the runtime's assemble_weights does, in Python: (weights, params, passes using Winograd)."""
    directory = out / native_dir(tier, up, profile)
    manifest = json.loads((directory / "fsr411_native_manifest.json").read_text())
    recipe = struct.unpack_from(f"<{manifest['weights_bytes']}I",
                                (directory / "fsr411_native_recipe.bin").read_bytes())
    weights = bytearray(initializer[r & ~GATHER] if r & GATHER else r for r in recipe)
    params = (directory / "fsr411_native_params.bin").read_bytes()
    lossy = importlib.util.spec_from_file_location("bb_fsr411_lossy", src / "tools/fsr411/lossy.py")
    module = importlib.util.module_from_spec(lossy)
    lossy.loader.exec_module(module)
    for entry in manifest["passes"].values():
        folding = entry.get("lossy_transform", {}).get("conv3x3_folding")
        if folding:
            start = entry["weights"][0]
            matrix = [v - 256 if v > 127 else v for v in weights[start:start + 2304]]
            folded, _ = module.fold_conv3x3(matrix, folding["drop_words_per_channel"])
            weights[start:start + 2304] = bytes(v & 255 for v in folded)
    accepted = set()
    for entry, first, end in winograd_regions(manifest):
        start = entry["weights"][0]
        values = winograd([v - 256 if v > 127 else v for v in weights[start:start + 2304]])
        if values:
            weights[first:end] = struct.pack(f"<{len(values)}h", *values)
            accepted.add(entry.get("logical_pass", 13))
    return bytes(weights), params, accepted, manifest


def verify(gen, gen_forced, src, out, tier, up, profile, init_dir, lossy2_weights):
    """The runtime's assembly against the upstream generator: the weights (the generator with its
    Winograd bounds lifted, which keeps the build's layout), the parameters and which passes use
    Winograd (the generator as it is)."""
    name = "initializers-ultraperformance.bin" if up else "initializers.bin"
    initializer = (init_dir / name).read_bytes()
    _, expected, expected_params = encode(gen_forced, src, tier, up, initializer, profile, lossy2_weights)
    real_manifest, _, _ = encode(gen, src, tier, up, initializer, profile, lossy2_weights)
    weights, params, accepted, manifest = assemble(src, out, tier, up, initializer, profile)
    decided = {e.get("logical_pass", 13) for e, _, _ in winograd_regions(real_manifest)}
    skip = set()
    for entry, first, end in winograd_regions(manifest):
        if entry.get("logical_pass", 13) not in accepted:
            skip.update(range(first, end))
    same = len(weights) == len(expected) and all(a == b for i, (a, b) in enumerate(zip(weights, expected)) if i not in skip)
    if not same or accepted != decided or params != expected_params:
        fail(f"{native_dir(tier, up, profile)}: the runtime's weights/parameters differ from the "
             f"generator's (Winograd passes {sorted(accepted)} vs {sorted(decided)})")
    print(f"FSR 4.1.x assets: {native_dir(tier, up, profile)}: the generator's exact weights and "
          f"parameters; Winograd in passes {sorted(accepted)}")


# ---------------------------------------------------------------------------------------------
# FSR 4.1.2 INT8 Lite: the model is imported from the builder's own AMD DLL by
# tools/fsr412_import_model.py (VENDOR.txt); these helpers build its Metal graph and native
# network from that imported model.

def load_lite_model(src, synthetic):
    path = src / "tools/fsr412/native_model.py"
    text = path.read_text()
    if synthetic:
        old = ("    if len(initializer)!=27392 or digest(initializer)!=expected:\n"
               "        raise ValueError('FSR 4.1.2 Lite initializer identity mismatch')")
        if text.count(old) != 1:
            fail("the 4.1.2 native model changed (initializer identity check)")
        text = text.replace(old, "    if False:\n        pass")
    module = types.ModuleType("bb_fsr412_native_model" + ("_synthetic" if synthetic else ""))
    module.__file__ = str(path)
    exec(compile(text, str(path), "exec"), module.__dict__)
    return module


def lite_pack(module, tier, up, initializer):
    with tempfile.TemporaryDirectory() as temp:
        out = Path(temp)
        module.pack(tier, "ultraperformance" if up else "normal", initializer, out)
        return (json.loads((out / "fsr411_native_manifest.json").read_text()),
                (out / "fsr411_native_weights.bin").read_bytes(),
                (out / "fsr411_native_params.bin").read_bytes())


def build_native_lite(module, out, tier, up):
    """The 4.1.2 network's recipe: weights that follow AMD's 27,392-byte Lite initializer are
    gathered; the Winograd regions are computed by the runtime; everything else is a constant."""
    runs = {name: lite_pack(module, tier, up, data) for name, data in (
        ("a", bytes([3]) * LITE_INIT_BYTES), ("b", bytes([9]) * LITE_INIT_BYTES),
        *((f"n{k}", bytes((i >> (4 * k)) & 15 for i in range(LITE_INIT_BYTES))) for k in range(5)))}
    manifest, weights_a, params = runs["a"]
    weights_b = runs["b"][1]
    for name, (other_manifest, weights, other) in runs.items():
        if other != params or len(weights) != len(weights_a) or \
                [r[1:] for r in winograd_regions(other_manifest)] != [r[1:] for r in winograd_regions(manifest)]:
            fail(f"the 4.1.2 native layout depends on the initializer ({name})")
    computed = set()
    for _, first, end in winograd_regions(manifest):
        computed.update(range(first, end))
    recipe = []
    for i, (a, b) in enumerate(zip(weights_a, weights_b)):
        if i in computed or a == b:
            recipe.append(0 if i in computed else a)
            continue
        if (a, b) != (3, 9):
            fail(f"4.1.2 weight byte {i} is not a copy of an initializer byte")
        offset = sum(runs[f"n{k}"][1][i] << (4 * k) for k in range(5))
        if offset >= LITE_INIT_BYTES:
            fail(f"4.1.2 weight byte {i} reads past the initializer")
        recipe.append(GATHER | offset)
    for entry in [*manifest["passes"].values(), *manifest.get("auxiliary_passes", {}).values()]:
        entry.pop("accumulator_bounds", None)
        entry.get("simd", {}).pop("float_accumulator_bound", None)
        entry.get("simd", {}).get("winograd", {}).pop("float_accumulator_bound", None)
    manifest["initializer_sha256"] = module.ULTRA_SHA if up else module.COMMON_SHA
    manifest["initializer_bytes"] = LITE_INIT_BYTES
    manifest.pop("weights_sha256", None)
    manifest.pop("params_sha256", None)
    manifest["weights_recipe"] = "fsr412_native_weights_recipe.bin"
    target = out / lite_native_dir(tier, up)
    target.mkdir(parents=True, exist_ok=True)
    (target / "fsr411_native_params.bin").write_bytes(params)
    (target / "fsr412_native_weights_recipe.bin").write_bytes(struct.pack(f"<{len(recipe)}I", *recipe))
    (target / "fsr411_native_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest, recipe


def assemble_lite(out, tier, up, initializer, profile="int8-lite"):
    directory = out / lite_native_dir(tier, up)
    manifest = json.loads((directory / "fsr411_native_manifest.json").read_text())
    recipe = struct.unpack_from(f"<{manifest['weights_bytes']}I",
                                (directory / "fsr412_native_weights_recipe.bin").read_bytes())
    weights = bytearray(initializer[r & ~GATHER] if r & GATHER else r for r in recipe)
    params = (directory / "fsr411_native_params.bin").read_bytes()
    accepted = set()
    for entry, first, end in winograd_regions(manifest):
        start = entry["weights"][0]
        values = winograd([v - 256 if v > 127 else v for v in weights[start:start + 2304]])
        if values:
            weights[first:end] = struct.pack(f"<{len(values)}h", *values)
            accepted.add(entry.get("logical_pass", 13))
    return bytes(weights), params, accepted, manifest


def verify_lite(module, out, tier, up, model_dir):
    name = "initializers-lite-ultraperformance.bin" if up else "initializers-lite.bin"
    initializer = (model_dir / name).read_bytes()
    real_manifest, expected, expected_params = lite_pack(module, tier, up, initializer)
    weights, params, accepted, manifest = assemble_lite(out, tier, up, initializer)
    decided = {e.get("logical_pass", 13) for e, _, _ in winograd_regions(real_manifest)}
    skip = set()
    for entry, first, end in winograd_regions(manifest):
        if entry.get("logical_pass", 13) not in accepted:
            skip.update(range(first, end))
    same = len(weights) == len(expected) and all(a == b for i, (a, b) in enumerate(zip(weights, expected)) if i not in skip)
    if not same or accepted != decided or params != expected_params:
        fail(f"{lite_native_dir(tier, up)}: the runtime's weights/parameters differ from the "
             f"4.1.2 model (Winograd passes {sorted(accepted)} vs {sorted(decided)})")
    print(f"FSR 4.1.x assets: {lite_native_dir(tier, up)}: the 4.1.2 model's exact weights and "
          f"parameters; Winograd in passes {sorted(accepted)}")


def prepare_lite_tree(src, work, model_dir):
    """compile_fsr412.py resolves its own tree (ROOT = parents[2]) and looks for its model,
    catalogue and support kernels under it; copy the tools and link the imported model."""
    tree = work / "fsr412_tree"
    if tree.exists():
        shutil.rmtree(tree)
    tree.mkdir(parents=True)
    shutil.copytree(src / "tools", tree / "tools", symlinks=False)
    os.symlink(src / "third_party", tree / "third_party")
    (tree / "build").mkdir()
    os.symlink(model_dir.resolve(), tree / "build" / "fsr412_model")
    return tree


def build_bindings_lite(src, out, work, tiers, model_dir):
    tree = prepare_lite_tree(src, work, model_dir)
    builddir = work / "fsr412_build"
    for tier in tiers:
        for ae in (0, 1):
            for up in (0, 1):
                v = VARIANT
                key = lite_variant_key(tier, ae, up)
                run([sys.executable, tree / "tools/fsr412/compile_fsr412.py", "--root", tree,
                     "--builddir", builddir, "--outdir", out / key, "--res", tier,
                     "--depth-inverted", v["depth_inverted"], "--low-res-mv", v["low_res_mv"],
                     "--jittered-mv", v["jittered_mv"], "--colorspace", v["colorspace"],
                     "--auto-exposure", ae, "--optimize", v["optimize"],
                     "--ultra-performance", up, "--external-post-cnb", 1])
                if not (out / key / "bindings.json").is_file():
                    fail(f"no 4.1.2 bindings for {key}")


def fetch_lossy2_weights(src, explicit):
    path = Path(explicit) if explicit else src / "build/fsr411_lossy2/weights_lossy2.npz"
    if path.is_file():
        return path
    if explicit:
        return None
    print("FSR 4.1.x assets: fetching the pinned GPL lossy2 weights (about 50 KB)")
    result = subprocess.run([sys.executable, str(src / "tools/fsr411/fetch_lossy2_weights.py")],
                            capture_output=True, text=True)
    if result.returncode:
        print("FSR 4.1.x assets: lossy2 weights unavailable; FSR 4.1 Lossy is not built\n"
              + result.stdout + result.stderr, file=sys.stderr)
        return None
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--src", type=Path, required=True, help="fsr4-native-macos tree (deps/share)")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--work", type=Path, help="compiler build folder (default: OUT/../fsr4_metal_work)")
    parser.add_argument("--tiers", type=int, nargs="+", default=[1080, 2160])
    parser.add_argument("--profiles", default="exact-fast",
                        help="comma list of the 4.1 profiles to build: exact-fast (default), lossy2")
    parser.add_argument("--lossy2-weights", type=Path, help="weights_lossy2.npz (default: fetch into SRC/build)")
    parser.add_argument("--fsr412-model", type=Path,
                        help="the imported FSR 4.1.2 model (tools/fsr412_import_model.py --with-shaders)")
    parser.add_argument("--verify", type=Path,
                        help="AMD's 4.1.1 initializers (default: SRC/third_party/fsr411)")
    parser.add_argument("--verify-412", type=Path,
                        help="the imported 4.1.2 model (default: --fsr412-model)")
    args = parser.parse_args()
    here = Path(__file__).resolve().parent
    src, out = args.src.resolve(), args.out.resolve()
    profiles = [p.strip() for p in args.profiles.split(",") if p.strip()]
    for profile in profiles:
        if profile not in PROFILE_SUFFIX:
            fail(f"unknown profile {profile!r} (exact-fast, lossy2)")
    work = (args.work or out.parent / "fsr4_metal_work").resolve()
    model = args.fsr412_model.resolve() if args.fsr412_model else None

    # Local verification of the recipes against the upstream generators.
    if args.verify or args.verify_412:
        gen = load_generator(src, synthetic=False)
        gen_forced = load_generator(src, synthetic=False, force_winograd=True)
        init_dir = (args.verify or src / "third_party/fsr411").resolve()
        lossy2 = fetch_lossy2_weights(src, args.lossy2_weights) if "lossy2" in profiles else None
        if "lossy2" in profiles and not lossy2:
            fail("lossy2 verification needs weights_lossy2.npz")
        for tier in args.tiers:
            for up in (0, 1):
                for profile in profiles:
                    verify(gen, gen_forced, src, out, tier, up, profile, init_dir, lossy2)
        if args.verify_412:
            lite = load_lite_model(src, synthetic=False)
            for tier in args.tiers:
                for up in (0, 1):
                    verify_lite(lite, out, tier, up, args.verify_412.resolve())
        return

    if out.exists():
        shutil.rmtree(out)  # only this script's output: nothing stale from an earlier upstream
    out.mkdir(parents=True, exist_ok=True)
    gen = load_generator(src, synthetic=True)
    lossy2 = fetch_lossy2_weights(src, args.lossy2_weights) if "lossy2" in profiles else None
    if "lossy2" in profiles and not lossy2:
        print("FSR 4.1.x assets: FSR 4.1 Lossy is not built", file=sys.stderr)
        profiles = [p for p in profiles if p != "lossy2"]
    build_kernels(here, out, args.tiers)
    build_bindings(src, out, work / "fsr411_build", args.tiers, profiles, lossy2)
    for tier in args.tiers:
        for up in (0, 1):
            for profile in profiles:
                manifest, recipe = build_native(gen, src, out, tier, up, profile, lossy2)
                gathered = sum(1 for r in recipe if r & GATHER)
                print(f"FSR 4.1.x assets: {native_dir(tier, up, profile)}: {len(recipe)} weight bytes, "
                      f"{gathered} from the initializer")
    if model:
        lite = load_lite_model(src, synthetic=True)
        build_bindings_lite(src, out, work, args.tiers, model)
        for tier in args.tiers:
            for up in (0, 1):
                manifest, recipe = build_native_lite(lite, out, tier, up)
                gathered = sum(1 for r in recipe if r & GATHER)
                print(f"FSR 4.1.x assets: {lite_native_dir(tier, up)}: {len(recipe)} weight bytes, "
                      f"{gathered} from the 4.1.2 initializer")
    else:
        print("FSR 4.1.x assets: no --fsr412-model; FSR 4.1.2 Lite is not built")


if __name__ == "__main__":
    main()

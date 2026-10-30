// SPDX-License-Identifier: MIT
// Shared implementation for the standalone runner and MetalFX live bridge.
// Compile Objective-C++ users with ARC. encode() never commits the caller's
// command buffer. One View is one ordered temporal stream / MetalFX scaler.
#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <CommonCrypto/CommonDigest.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "fsr4_input.h"
#include "fsr4_log.h"
#include "fsr411_model.h"
#include "fsr411_scratch_layout.h"

namespace fsr411 {

// Simd: Apple7+ (M1–M4) simdgroup half matrices with exact float accumulation;
// byte-identical to Scalar. Auto: Native on Apple10, else Simd, else Scalar.
enum class Backend { Auto, Reference, Native, Scalar, Simd };
// Integer network implementation chosen for one session.
enum class Kernels { Reference, Scalar, Simd, Mpp };
inline const char *kernels_label(Kernels k) {
    return k==Kernels::Mpp?"int8-mpp":k==Kernels::Simd?"int8-simd":k==Kernels::Scalar?"int8-scalar":"int8-reference";
}
// Exact: original coefficients, IEEE PRE/POST math, inference every frame.
// ExactFast: Exact's network with reciprocal division and fast exp/log in
// PRE/POST. Lossy: folded coefficients, half-up rounding and inference reuse.
// Lossy2: Lossy's network with the six layers trained in upstream dll-2026-10-19.
enum class Profile { Exact, Lossy, ExactFast, Lossy2, Int8Lite };
inline bool profile_is_lossy(Profile p) { return p==Profile::Lossy || p==Profile::Lossy2; }
inline const char *profile_name(Profile p) {
    return p==Profile::Int8Lite?"int8-lite":p==Profile::Lossy?"lossy":p==Profile::Lossy2?"lossy2":p==Profile::ExactFast?"exact-fast":"exact";
}
inline bool parse_profile(const std::string &name, Profile &p) {
    if(name=="exact") p=Profile::Exact;
    else if(name=="exact-fast") p=Profile::ExactFast;
    else if(name=="lossy") p=Profile::Lossy;
    else if(name=="lossy2") p=Profile::Lossy2;
    else return false;
    return true;
}
// Live settings encoding (panel.conf, fsr4_live_get_profile): 0/1 keep their
// original meaning so older settings files still load.
inline Profile profile_from_int(int v) { return v==1?Profile::Lossy:v==2?Profile::ExactFast:v==4?Profile::Lossy2:Profile::Exact; }
inline int profile_to_int(Profile p) { return p==Profile::Lossy?1:p==Profile::ExactFast?2:p==Profile::Lossy2?4:0; }
// Synchronous, opt-in CLI instrumentation. A profiler may replace the classic
// compute encoder to attach stage-boundary counters. Ordinary/live encodes do
// not install one, retain their single encoder, and record no counter samples.
struct StageProfiler {
    virtual ~StageProfiler() = default;
    virtual void begin_stage(id __strong &encoder, const std::string &name) = 0;
    virtual void end_stage(id encoder) = 0;
};
struct FrameSnapshot {
    Fsr4InputDesc input = {};
    Fsr4InputPrepared prepared = {};
    // Logical input/output regions, with source format values preserved.
    // A fresh snapshot belongs to one encode; its completion token never resets.
    id<MTLTexture> color = nil, depth = nil, motion = nil, output = nil;
    id<MTLBuffer> completion = nil;
    bool ready() const {
        return completion && __atomic_load_n((uint32_t *)completion.contents, __ATOMIC_ACQUIRE) == 1u;
    }
};
struct Options {
    // The 4.1.2 adapter selects its own imported graph and cache namespace.
    // Input conversion, resource lifetime and encoder synchronization are shared.
    bool modelLite = false;
    Backend backend = Backend::Auto;
    Profile profile = Profile::Lossy;
    int tier = 0, colorspace = 0, optimize = 1;
    // bbport: no allowCompile: nothing is built while the game runs (build_assets.py).
    bool rcas = false, ultraPerformance = false;
    // FSR 4.1.2 only: widen POST's current-frame filter where history is rejected.
    bool pixelFix = false;
    // Lower bound of the widening, 0..1 (1 = off); POST reads it from `pad`.
    float pixelFixFloor = 0.6f;
    // Tests only: Exact Fast/Lossy PRE/POST with ordered arithmetic, so their
    // optimized graph can be compared bit for bit with the imported HLSL.
    bool strictMath = false;
    float sharpness = 0;
    // Diagnostic snapshots are GPU copies at a precise graph boundary.
    // They are not enabled by the live bridge or ordinary benchmarking.
    std::vector<std::string> captureStages;
    // Optional immutable CPU-readable frame. No allocation or dispatch when null.
    std::shared_ptr<FrameSnapshot> snapshot;
    StageProfiler *stageProfiler = nullptr; // caller owns it until encode returns
};
struct Frame {
    id<MTLTexture> color = nil, depth = nil, motion = nil, output = nil, exposure = nil;
    Fsr4InputDesc input = {};
};

struct FrameConstants {
    float inv_size[2], scale[2], inv_scale[2], jitter[2], mv_scale[2];
    float tex_size[2], max_renderSize[2], jitterCancellation[2];
    uint32_t width, height, reset, width_lr;
    uint32_t height_lr; float preExposure, previousPreExposure; uint32_t rcas;
    float sharpness, pad;
};
struct SpdConstants {
    uint32_t mips, groups, offset[2];
    float invInputSize[2], preExposure, padding;
    uint32_t validSize[2]; // 4.1.1 reimplementation's viewport extension
};
struct RcasConstants { uint32_t config[4]; float preExposure; uint32_t pad[3]; };
struct LossyConstants { uint32_t skip, blockWidth, blockHeight, reserved; };
struct RegionConstants { uint32_t source[4], destination[4]; };
struct TensorSizes { uint32_t v[17][4]; };
static_assert(sizeof(FrameConstants) == 104 && sizeof(SpdConstants) == 40,
              "FSR 4.1.1 frame constant ABI");
static_assert(sizeof(TensorSizes) == 272 && sizeof(RcasConstants) == 32,
              "FSR 4.1.1 tensor/RCAS ABI");

inline uint32_t ceil_div(uint32_t x, uint32_t y) { return (x + y - 1u) / y; }
inline NSString *ns(const std::string &x) { return [NSString stringWithUTF8String:x.c_str()]; }
inline std::string utf8(NSString *x) { return x ? std::string(x.UTF8String) : std::string(); }
inline std::string metal_error(NSError *error) { return error ? utf8(error.localizedDescription) : "Metal allocation failed"; }
// The compiler publishes bindings.json after each stage. Its presence alone
// does not mean an interrupted or --only build is ready for --no-compile.
// This checks completeness only; layout/key/order validation stays in the
// loader so a complete but incompatible alias bundle is still rejected.
inline bool shader_bundle_files_ready(const std::string &directory) {
    NSString *dir=ns(directory);
    NSData *data=[NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"bindings.json"]];
    id manifest=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:nil]:nil;
    if(![manifest isKindOfClass:[NSDictionary class]]) return false;
    NSDictionary *entries=manifest[@"kernels"];
    if(![entries isKindOfClass:[NSDictionary class]]) return false;
    for(unsigned index=0;index<27;++index) {
        const std::string name=index==0?"pre":index==26?"post":
            std::string(index&1?"pad":"pass")+std::to_string(index/2);
        if(![entries[ns(name)] isKindOfClass:[NSDictionary class]]) return false;
    }
    if(!entries[@"ae"] || !entries[@"rcas"]) return false;
    for(NSString *flag in @[@"auto_exposure_dispatches",@"separate_reuse_pre"])
        if(manifest[flag] && ![manifest[flag] isKindOfClass:[NSNumber class]]) return false;
    if([manifest[@"auto_exposure_dispatches"] unsignedIntValue]==2 && !entries[@"ae_finalize"]) return false;
    if([manifest[@"separate_reuse_pre"] boolValue] && !entries[@"pre_reuse"]) return false;
    NSFileManager *files=[NSFileManager defaultManager];
    auto readyFile=[&](id name) {
        if(![name isKindOfClass:[NSString class]] || ![name length]) return false;
        NSString *path=[[dir stringByAppendingPathComponent:name] stringByResolvingSymlinksInPath];
        NSDictionary *attributes=[files attributesOfItemAtPath:path error:nil];
        return [attributes[NSFileType] isEqual:NSFileTypeRegular] &&
            [attributes[NSFileSize] unsignedLongLongValue]>0 && [files isReadableFileAtPath:path];
    };
    // Inspect every entry: the loader also loads optional helpers. Requiring
    // generated pass files permits its fallback when a native pass is absent.
    for(NSString *name in entries) {
        NSDictionary *entry=entries[name];
        if(![entry isKindOfClass:[NSDictionary class]] || !readyFile(entry[@"library"]) ||
           ![entry[@"entry"] isKindOfClass:[NSString class]] || ![entry[@"entry"] length]) return false;
        NSArray *wg=entry[@"wgsize"];
        if(![wg isKindOfClass:[NSArray class]] || wg.count!=3) return false;
        for(id size in wg) if(![size isKindOfClass:[NSNumber class]] || [size longLongValue]<=0) return false;
        bool needsTables=false;
        for(NSString *kind in @[@"buffers",@"textures",@"samplers"]) {
            NSArray *bindings=entry[kind];
            if(![bindings isKindOfClass:[NSArray class]]) return false;
            for(NSArray *binding in bindings) {
                if(![binding isKindOfClass:[NSArray class]] || binding.count!=2 ||
                   ![binding[0] isKindOfClass:[NSNumber class]] || [binding[0] longLongValue]<0 ||
                   ![binding[1] isKindOfClass:[NSString class]]) return false;
                needsTables |= [kind isEqual:@"buffers"] && [binding[1] isEqual:@"Fsr411Tables"];
            }
        }
        if((needsTables || entry[@"literal_tables"]) && !readyFile(entry[@"literal_tables"])) return false;
    }
    return true;
}
inline TensorSizes tensor_sizes(uint32_t w, uint32_t h) {
    // Provider tensor constants are indexed by graph edges, not by passes.
    constexpr unsigned shifts[17] = {1,0,1,1,2,2,2,3,3,3,2,2,1,1,0,0,0};
    TensorSizes result = {};
    for (int i = 0; i < 17; ++i) { result.v[i][0] = w >> shifts[i]; result.v[i][1] = h >> shifts[i]; }
    return result;
}
inline int required_tier(uint32_t w, uint32_t h) {
    if (w <= 1920 && h <= 1080) return 1080;
    if (w <= 3840 && h <= 2160) return 2160;
    if (w <= 7680 && h <= 4320) return 4320;
    return 0;
}
inline int pass_shift(int pass) {
    if (pass <= 2 || pass == 12) return 1;
    if (pass >= 6 && pass <= 9) return 3;
    return 2;
}
inline int pad_shift(int pad) {
    if (pad <= 2 || pad >= 11) return 1;
    if (pad >= 6 && pad <= 8) return 3;
    return 2;
}
inline uint32_t padding_groups(int tier, int pad, uint32_t uw, uint32_t uh) {
    const int shift = pad_shift(pad);
    const uint32_t w = uw >> shift, h = uh >> shift;
    const uint32_t maxW = uint32_t(tier * 16 / 9) >> shift, maxH = uint32_t(tier) >> shift;
    const uint32_t c = std::min(maxW + 1u - w, 5u), d = std::min(maxH + 1u - h, 1u);
    const uint32_t f = w + 1u + c;
    return ceil_div(f + h + h * c + f * d, 32);
}

struct Kernel {
    id<MTLComputePipelineState> pso = nil;
    id<MTLBuffer> literalTables = nil;
    MTLSize threads = MTLSizeMake(1, 1, 1);
    std::vector<std::pair<NSUInteger, std::string>> buffers, textures, samplers;
    int tensorSlot = -1;
    unsigned tileX = 1, tileY = 1;
};
struct NativePass {
    id<MTLComputePipelineState> pso = nil;
    PassParams parameters = {};
    unsigned inputSlot = 0, outputSlot = 0, skipSlot = 0;
    unsigned tileX = 1, tileY = 1, threads = 128, offset = 0;
    bool inputDispatch = false, hasSkip = false;
};
struct Session {
    std::string key;
    int tier = 0;
    uint32_t postPixelsPerGroup = 32;
    bool splitAutoExposure = false;
    bool modelLite = false;
    size_t scratchBytes = 0;
    bool preparedColor = false;
    bool ultraPerformance = false, externalPostCnb = false;
    bool postCnbAlias = false;
    PostCnbScratchLayout scratchLayout = {};
    Profile profile = Profile::Exact;
    // halfWeights: Simd only, nativeWeights widened element-wise to half.
    id<MTLBuffer> initializer = nil, nativeWeights = nil, halfWeights = nil;
    std::map<std::string, Kernel> kernels;
    std::map<int, NativePass> native;
    std::map<std::string,std::vector<int>> nativeSequence;
    std::vector<std::string> order;
};

struct FrameSlot {
    static constexpr size_t kSize = 32768, kDone = kSize - 4;
    id<MTLBuffer> constants = nil;
    NSMutableDictionary *tables = nil;
    NSMutableArray *resources = nil;
    id<MTLResidencySet> residency = nil;
    std::shared_ptr<FrameSnapshot> snapshot;
    StageProfiler *stageProfiler = nullptr;
    bool busy = false;
    bool retired() const {
        return !busy || (constants && __atomic_load_n(
            (uint32_t *)((uint8_t *)constants.contents + kDone), __ATOMIC_ACQUIRE) == 1);
    }
};

struct View {
    Fsr4InputHistory inputHistory = {};
    Fsr4InputPrepared prepared = {};
    std::string variantKey, backendLabel;
    bool postCnbAlias = false;
    PostCnbScratchLayout scratchLayout = {};
    id<MTLTexture> history = nil, reprojected = nil, recurrent = nil, autoExposure = nil;
    id<MTLBuffer> scratch = nil, spdAtomic = nil;
    id<MTLBuffer> lossyMotion = nil, lossyPlane = nil;
    id<MTLTexture> spdAtomicTexture = nil, luma = nil;
    id<MTLTexture> colorCopy = nil, depthCopy = nil, motionCopy = nil, outputCopy = nil, rcasTemp = nil;
    id<MTLTexture> preparedColor = nil;
    std::map<std::string, id<MTLBuffer>> captures;
    std::vector<std::unique_ptr<FrameSlot>> slots;
    uint32_t width = 0, height = 0;
    uint64_t encodes = 0;
    uint64_t neuralRuns = 0, reusedFrames = 0;
    bool cacheValid = false, skippedInference = false;
    bool zeroScratch = true, zeroTargets = true;
    void invalidate() { fsr4_input_invalidate(&inputHistory);cacheValid=false; }
};

class Runtime {
public:
    // bbport: `root` holds what gpu/fsr4_metal/build_assets.py built (metallibs, bindings,
    // native manifests and weight recipes); `initializerDirs` are where the player keeps AMD's
    // initializers, searched in order. Nothing is compiled or written while the game runs.
    Runtime(id<MTLDevice> dev, const std::string &root, std::vector<std::string> initializerDirs)
        : device_(dev), root_(root), initializerDirs_(std::move(initializerDirs)) {}

    // bbport: the folder of the built native network for a tier, preset and model.
    // FSR 4.1 exact-fast and 4.1.2 Lite have no suffix; 4.1 Lossy (lossy2) has "_lossy2".
    static std::string native_dir(int tier, bool ultraPerformance, bool modelLite, Profile profile) {
        return (modelLite ? "fsr412_native_" : "fsr411_native_") + std::to_string(tier)
            + (ultraPerformance ? "_ultraperformance" : "")
            + (!modelLite && profile == Profile::Lossy2 ? "_lossy2" : "");
    }
    static std::string sha256_hex(NSData *data) {
        unsigned char digest[CC_SHA256_DIGEST_LENGTH];
        CC_SHA256(data.bytes, (CC_LONG)data.length, digest);
        char hex[2 * CC_SHA256_DIGEST_LENGTH + 1];
        for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) snprintf(hex + 2 * i, 3, "%02x", digest[i]);
        return hex;
    }
    // bbport: AMD's initializer for a model, from the player's folders (also the Vulkan FSR
    // 4.1.1 path's asset sets, t<tier>_m<model>/initializer.bin), accepted only with the SHA-256
    // the native manifest names: the exact file its weight recipe was checked against. The
    // 4.1/4.1-Lossy networks share the 131,072-byte 4.1.1 pair; 4.1.2 Lite has its own
    // 27,392-byte pair.
    static NSData *find_initializer(const std::string &root, const std::vector<std::string> &dirs,
                                    bool ultraPerformance, bool modelLite, std::string &error) {
        const std::string name = modelLite
            ? (ultraPerformance ? "initializers-lite-ultraperformance.bin" : "initializers-lite.bin")
            : (ultraPerformance ? "initializers-ultraperformance.bin" : "initializers.bin");
        NSString *expected = nil;
        for (const std::string &native : {native_dir(1080, ultraPerformance, modelLite, Profile::ExactFast),
                                          native_dir(1080, ultraPerformance, modelLite, Profile::Lossy2)}) {
            NSData *manifestData = [NSData dataWithContentsOfFile:ns(root + "/" + native + "/fsr411_native_manifest.json")];
            if (!manifestData) continue;
            NSDictionary *manifest = [NSJSONSerialization JSONObjectWithData:manifestData options:0 error:nil];
            if ([manifest isKindOfClass:[NSDictionary class]] && manifest[@"initializer_sha256"]) {
                expected = manifest[@"initializer_sha256"];
                break;
            }
        }
        if (!expected) {
            error = "the FSR " + std::string(modelLite ? "4.1.2" : "4.1") + " Metal files were not built (" + root + ")";
            return nil;
        }
        const size_t size = modelLite ? 27392 : 131072;
        bool wrong = false;
        for (const auto &dir : dirs) {
            std::vector<std::string> files = {
                dir + "/" + name,
                dir + "/" + (modelLite ? "fsr4_412" : "fsr4_411") + "/" + name,
            };
            if (!modelLite) {
                const std::string model = ultraPerformance ? "_m1" : "_m0";
                files.push_back(dir + "/t1080" + model + "/initializer.bin");
                files.push_back(dir + "/t2160" + model + "/initializer.bin");
            }
            for (const std::string &file : files) {
                NSData *data = [NSData dataWithContentsOfFile:ns(file)];
                if (!data) continue;
                if (data.length == size && sha256_hex(data) == utf8(expected)) return data;
                wrong = true;
            }
        }
        const std::string version = modelLite ? "4.1.2" : "4.1.1.2740";
        error = wrong ? "AMD's " + name + " is not the FSR " + version + " one"
                      : "AMD's " + name + " is missing (tools/fsr4_metal_initializers.sh from your AMD DLL)";
        return nil;
    }
    // bbport: everything a mode's session needs is there (the menu offers it only then).
    static bool assets_ready(const std::string &root, const std::vector<std::string> &dirs411,
                             const std::vector<std::string> &dirs412, Profile mode, std::string &error) {
        auto exists = [](const std::string &p) { return [[NSFileManager defaultManager] fileExistsAtPath:ns(p)]; };
        const bool modelLite = mode == Profile::Int8Lite;
        const std::string version = modelLite ? "4.1.2" : mode == Profile::Lossy2 ? "4.1 Lossy" : "4.1";
        for (const char *name : {"/fsr411_helpers.metallib", "/fsr411_simd.metallib"}) {
            if (!exists(root + name)) { error = "the FSR " + version + " Metal files were not built (" + root + ")"; return false; }
        }
        const std::string key = modelLite
            ? "fsr412_1080_di0_lm1_jm0_cs0_ae1_o1_up0_pc1_v2"
            : std::string("fsr411_1080_di0_lm1_jm0_cs0_ae1_o1")
                  + (mode == Profile::Lossy2 ? "_lossy2" : "_fast") + "_pc1_ca1";
        const std::string native = native_dir(1080, false, modelLite, mode);
        const std::string weights = modelLite ? "/fsr412_native_weights_recipe.bin" : "/fsr411_native_recipe.bin";
        const std::string params = "/fsr411_native_params.bin";
        for (const std::string &path : {"/" + key + "/bindings.json", "/" + native + "/fsr411_native_manifest.json",
                                        "/" + native + weights, "/" + native + params}) {
            if (!exists(root + path)) { error = "the FSR " + version + " Metal files were not built (" + root + ")"; return false; }
        }
        return find_initializer(root, modelLite ? dirs412 : dirs411, false, modelLite, error) != nil;
    }

    // bbport: the native weights are bytes of AMD's initializer and constants of the MIT HLSL
    // import, in the order the generator (gen_fsr411_native.py) writes them. The recipe has one
    // word per byte (bit 31: the initializer byte at the low bits, else the byte in the low 8
    // bits); then the Lossy folding of the passes that have it (the generator's
    // tools/fsr411/lossy.py fold_conv3x3) and the simd Winograd weights (its winograd()), which
    // are computed from those values. build_assets.py --verify checks this against the generator.
    static void fold_conv3x3(int8_t *m, int dropWords) {
        for (int output = 0; output < 16; ++output) {
            int vectors[36][4], index[36][4], magnitude[36];
            for (int tap = 0; tap < 9; ++tap) for (int q = 0; q < 4; ++q) {
                const int v = tap * 4 + q; magnitude[v] = 0;
                for (int c = 0; c < 4; ++c) {
                    index[v][c] = (tap * 16 + q * 4 + c) * 16 + output;
                    vectors[v][c] = m[index[v][c]]; magnitude[v] += vectors[v][c] * vectors[v][c];
                }
            }
            int ordered[36]; for (int i = 0; i < 36; ++i) ordered[i] = i;
            std::stable_sort(ordered, ordered + 36, [&](int a, int b) { return magnitude[a] < magnitude[b]; });
            bool survives[36]; std::fill(survives, survives + 36, true);
            int remaining[4] = {9, 9, 9, 9}; std::vector<int> removed;
            for (int v : ordered) {
                if (int(removed.size()) >= dropWords) break;
                if (magnitude[v] && remaining[v % 4] > 1) { survives[v] = false; --remaining[v % 4]; removed.push_back(v); }
            }
            auto distance = [](int a, int b) {
                const int ay = (a / 4) / 3, ax = (a / 4) % 3, by = (b / 4) / 3, bx = (b / 4) % 3;
                return (ay - by) * (ay - by) + (ax - bx) * (ax - bx);
            };
            for (int source : removed) {
                int target = -1;
                for (int v = source % 4; v < 36; v += 4) {
                    if (!survives[v]) continue;
                    if (target < 0 || distance(source, v) < distance(source, target)
                        || (distance(source, v) == distance(source, target) && magnitude[v] > magnitude[target])) target = v;
                }
                for (int c = 0; c < 4; ++c) {
                    vectors[target][c] = std::clamp(vectors[target][c] + vectors[source][c], -128, 127);
                    vectors[source][c] = 0;
                }
            }
            for (int v = 0; v < 36; ++v) for (int c = 0; c < 4; ++c) m[index[v][c]] = int8_t(vectors[v][c]);
        }
    }
    // Integer Winograd F(2x2,3x3): w' = (2G) g (2G)^T per [position 4*xi+nu][input][output];
    // false unless it is exact in half with float sums (the generator's bounds).
    static bool winograd(const int8_t *conv, int16_t *out) {
        static const int g2[4][3] = {{2, 0, 0}, {1, 1, 1}, {1, -1, 1}, {0, 0, 2}};
        static const int at[2][4] = {{1, 1, 1, 0}, {0, 1, -1, -1}};
        std::vector<int> values(4096);
        for (int xi = 0; xi < 4; ++xi) for (int nu = 0; nu < 4; ++nu)
            for (int ic = 0; ic < 16; ++ic) for (int oc = 0; oc < 16; ++oc) {
                int sum = 0;
                for (int ky = 0; ky < 3; ++ky) for (int kx = 0; kx < 3; ++kx)
                    sum += g2[xi][ky] * g2[nu][kx] * conv[(ky * 3 + kx) * 256 + ic * 16 + oc];
                values[((xi * 4 + nu) * 16 + ic) * 16 + oc] = sum;
            }
        for (int v : values) if (std::abs(v) > 2048) return false;
        long long worst = 0;
        for (int oc = 0; oc < 16; ++oc) {
            long long column[16];
            for (int p = 0; p < 16; ++p) {
                long long sum = 0;
                for (int ic = 0; ic < 16; ++ic) sum += std::abs(values[(p * 16 + ic) * 16 + oc]);
                column[p] = 512 * sum;
            }
            for (int i = 0; i < 2; ++i) for (int j = 0; j < 2; ++j) {
                long long sum = 0;
                for (int xi = 0; xi < 4; ++xi) for (int nu = 0; nu < 4; ++nu)
                    if (at[i][xi] && at[j][nu]) sum += column[xi * 4 + nu];
                worst = std::max(worst, sum);
            }
        }
        if (worst >= (1ll << 24)) return false;
        for (int i = 0; i < 4096; ++i) out[i] = int16_t(values[i]);
        return true;
    }
    // `manifest` is mutable: a pass whose Winograd weights would not be exact loses its
    // "winograd" dispatch (direct kernel), as the generator would not have written one.
    static NSData *assemble_weights(NSMutableDictionary *manifest, NSData *recipe, NSData *initializer, std::string &error) {
        const size_t count = [manifest[@"weights_bytes"] unsignedLongLongValue];
        if (!count || recipe.length != count * 4) { error = "the native 4.1.1 weight recipe is incomplete"; return nil; }
        NSMutableData *weights = [NSMutableData dataWithLength:count];
        auto *out = (uint8_t *)weights.mutableBytes;
        const auto *words = (const uint32_t *)recipe.bytes;
        const auto *init = (const uint8_t *)initializer.bytes;
        for (size_t i = 0; i < count; ++i) {
            const uint32_t w = words[i];
            if (w & 0x80000000u) {
                const uint32_t offset = w & 0x7fffffffu;
                if (offset >= initializer.length) { error = "the native 4.1.1 weight recipe reads past the initializer"; return nil; }
                out[i] = init[offset];
            } else out[i] = uint8_t(w);
        }
        NSMutableArray *entries = [[manifest[@"passes"] allValues] mutableCopy];
        for (NSDictionary *entry in entries) {
            NSDictionary *folding = entry[@"lossy_transform"][@"conv3x3_folding"];
            if (!folding) continue;
            const size_t start = [entry[@"weights"][0] unsignedLongLongValue];
            if (start + 2304 > count) { error = "native 4.1.1 folding outside the weights"; return nil; }
            fold_conv3x3((int8_t *)out + start, [folding[@"drop_words_per_channel"] intValue]);
        }
        if (manifest[@"auxiliary_passes"][@"post_cnb"]) [entries addObject:manifest[@"auxiliary_passes"][@"post_cnb"]];
        for (NSMutableDictionary *entry in entries) {
            NSDictionary *w = entry[@"simd"][@"winograd"];
            if (!w) continue;
            const size_t start = [entry[@"weights"][0] unsignedLongLongValue];
            const size_t at = [w[@"weights0"] unsignedLongLongValue];
            if ([w[@"weights0_count"] unsignedLongLongValue] != 4096 || start + 2304 > count || at + 8192 > count) {
                error = "native 4.1.1 Winograd weights out of range"; return nil;
            }
            int16_t values[4096];
            if (winograd((const int8_t *)out + start, values)) memcpy(out + at, values, sizeof values);
            else [entry[@"simd"] removeObjectForKey:@"winograd"];
        }
        return weights;
    }
    bool copy_output(id cb, View &lifetime, id<MTLTexture> source, Fsr4InputRect sourceRect,
                     id<MTLTexture> output, Fsr4InputRect outputRect, std::string &error) {
        if (!source || !output || sourceRect.width != outputRect.width || sourceRect.height != outputRect.height
            || sourceRect.x > source.width || sourceRect.y > source.height
            || sourceRect.width > source.width-sourceRect.x || sourceRect.height > source.height-sourceRect.y
            || outputRect.x > output.width || outputRect.y > output.height
            || outputRect.width > output.width-outputRect.x || outputRect.height > output.height-outputRect.y) {
            error="placeholder encode has no compatible completed FSR output"; return false;
        }
        if (source == output && sourceRect.x==outputRect.x && sourceRect.y==outputRect.y) return true;
        if (source == output) { error="overlapping placeholder output copy requires a separate texture";return false; }
        if (!ensure_helpers(error)) return false;
        auto slot=acquire_slot(lifetime,error); if(!slot) return false;
        [slot->resources addObjectsFromArray:@[slot->constants,source,output]];
        id enc=[cb computeCommandEncoder]; if(!enc) { error="caller command buffer already encoding"; return false; }
        bool m4=![enc respondsToSelector:@selector(setBuffer:offset:atIndex:)];
        if (m4 && (!table(*slot,"copyOnly",error) || !table(*slot,"retire",error) || !bind_residency(*slot,cb,error))) {
            [enc endEncoding]; return false;
        }
        slot->busy=true;
        __atomic_store_n((uint32_t *)((uint8_t *)slot->constants.contents+FrameSlot::kDone),0u,__ATOMIC_RELEASE);
        @try {
            if(m4) queue_before(enc);
            copy(enc,m4,*slot,"copyOnly",source,output,sourceRect,outputRect.x,outputRect.y,8192,false);
            helper(enc,m4,*slot,"retire","fsr411_retire",{{slot->constants,FrameSlot::kDone}},{},MTLSizeMake(1,1,1));
            if(m4) queue_after(enc);
            [enc endEncoding]; return true;
        } @catch(NSException *exception) {
            @try { [enc endEncoding]; } @catch(NSException *) {}
            error=utf8(exception.reason); return false;
        }
    }

    bool encode(id commandBuffer, View &view, const Frame &frame, const Options &options, std::string &error) {
        error.clear();
        auto fail = [&](const std::string &why) { error = why; view.invalidate(); return false; };
        if (!device_ || !commandBuffer) return fail("missing Metal device or command buffer");
        if (!validate_textures(frame, error)) return fail(error);
        if (options.colorspace < 0 || options.colorspace > 3 || !std::isfinite(options.sharpness)
            || options.sharpness < 0 || options.sharpness > 1) return fail("invalid colour space or sharpness");
        Fsr4InputPrepared p = {};
        auto status = fsr4_input_prepare(&frame.input, &view.inputHistory, 0, &p);
        if (!fsr4_input_status_ok(status)) return fail(fsr4_input_status_reason(status));
        int tier = options.tier ? options.tier : required_tier(p.alignedUW, p.alignedUH);
        if ((tier != 1080 && tier != 2160 && tier != 4320) || !required_tier(p.alignedUW, p.alignedUH)
            || p.alignedUW > uint32_t(tier * 16 / 9) || p.alignedUH > uint32_t(tier))
            return fail("output content exceeds FSR 4.1.1 tensor tier");
        const bool native = (options.backend==Backend::Auto || options.backend==Backend::Native) && native_capable();
        if (options.backend == Backend::Native && !native) return fail("native FSR 4.1.1 requires Apple GPU family 10 and macOS 26");
        if (options.backend == Backend::Simd && !simd_family()) return fail("simd FSR 4.1.1 requires Apple GPU family 7 (M1) or newer");
        // bbport: M5 kernels only when this build has them (Xcode with Metal 4), else simd.
        const bool mppBuilt = [[NSFileManager defaultManager] fileExistsAtPath:
            ns(root_ + "/fsr411_mpp_" + std::to_string(tier) + ".metallib")];
        if (options.backend == Backend::Native && !mppBuilt) return fail("this build has no M5 (Metal 4) FSR 4.1.1 kernels");
        Kernels kind = native && mppBuilt ? Kernels::Mpp : options.backend==Backend::Reference ? Kernels::Reference
            : options.backend==Backend::Scalar ? Kernels::Scalar
            : (options.backend==Backend::Simd || (simd_family() && !simdFailed_)) ? Kernels::Simd : Kernels::Scalar;
        if (!ensure_helpers(error)) return fail(error);
        Session *session = options.modelLite ? ensure_lite_session(tier,p,options,kind,error)
                                             : ensure_session(tier,p,options,kind,error);
        if (!session && kind==Kernels::Simd && options.backend==Backend::Auto) {
            // Never retry a failed simd setup on this device; scalar is exact too.
            fsr4_logf("FSR 4.1.1 simd backend unavailable (%s); using scalar", error.c_str());
            simdFailed_ = true; kind = Kernels::Scalar; error.clear();
            session = options.modelLite ? ensure_lite_session(tier,p,options,kind,error)
                                        : ensure_session(tier,p,options,kind,error);
        }
        if (!session) return fail(error);
        const bool native_or_portable = kind != Kernels::Reference;
        status = fsr4_input_prepare(&frame.input, &view.inputHistory, view.variantKey != session->key, &p);
        if (!fsr4_input_status_ok(status)) return fail(fsr4_input_status_reason(status));
        if (!ensure_targets(view, *session, p, frame, options, error)) return fail(error);
        if (options.snapshot && !prepare_snapshot(*options.snapshot, frame, p, error)) return fail(error);
        FrameSlot *slot = acquire_slot(view, error);
        if (!slot) return fail(error);
        slot->snapshot = options.snapshot;
        slot->stageProfiler = options.stageProfiler;
        const bool rcas = options.rcas && options.sharpness > 0;
        const bool lossy = profile_is_lossy(options.profile);
        // The supplied vectors describe one frame of motion. Never apply
        // them directly to a cache older than one frame (unusual jitter can
        // have consecutive negatives). Refresh after every reused frame.
        const bool skip = lossy && view.cacheValid && !view.skippedInference && !p.reset && p.jitter[0]<0.f
            && uint64_t(p.rw)*2u>=p.alignedUW && !options.ultraPerformance;
        TensorSizes sizes = tensor_sizes(p.alignedUW, p.alignedUH);
        if(options.modelLite) {
            constexpr unsigned shifts[9]={1,0,1,2,2,1,0,0,0};
            sizes={};
            for(unsigned i=0;i<9;++i) {sizes.v[i][0]=p.alignedUW>>shifts[i];sizes.v[i][1]=p.alignedUH>>shifts[i];}
        }
        fill_constants(*slot, *session, p, sizes, rcas, options.sharpness,
                       options.pixelFix ? std::clamp(options.pixelFixFloor, 0.f, 1.f) : 0.f);
        const LossyConstants lc={skip?1u:0u,p.alignedUW/4u,p.alignedUH/4u,0};
        put(*slot,kLossy,lc);
        view.captures.clear();
        for (const auto &name : options.captureStages) {
            if (std::find(session->order.begin(),session->order.end(),name)==session->order.end()) return fail("unknown capture stage " + name);
            if(skip && name!="pre" && name!="post") continue;
            auto buffer = [device_ newBufferWithLength:session->scratchBytes options:MTLResourceStorageModeShared];
            if (!buffer) return fail("cannot allocate diagnostic scratch snapshot");
            view.captures.emplace(name, buffer);
        }
        id enc = [commandBuffer computeCommandEncoder];
        if (!enc) return fail("caller command buffer already has an active encoder");
        const bool m4 = ![enc respondsToSelector:@selector(setBuffer:offset:atIndex:)];
        if (m4 && options.stageProfiler) {
            [enc endEncoding]; return fail("CLI stage counters require a classic Metal command buffer");
        }
        if (m4 && ![enc respondsToSelector:@selector(setArgumentTable:)]) {
            [enc endEncoding]; return fail("unsupported Metal compute encoder");
        }
        // Reserve all argument tables before recording the first GPU write.
        if (m4 && !prepare_tables(*slot, *session, view, error)) {
            [enc endEncoding]; return fail(error);
        }
        retain_resources(*slot, *session, view, frame);
        if (m4 && !bind_residency(*slot, commandBuffer, error)) {
            [enc endEncoding]; return fail(error);
        }
        slot->busy = true;
        __atomic_store_n((uint32_t *)((uint8_t *)slot->constants.contents + FrameSlot::kDone), 0u, __ATOMIC_RELEASE);
        Bindings b{frame.color, frame.depth, frame.motion, frame.output,
                   p.exposureMode == FSR4_INPUT_EXPOSURE_TEXTURE ? frame.exposure : exposureOne_, &view, rcas};
        @try {
            if (m4) queue_before(enc);
            if (view.zeroScratch) clear_buffer(enc, m4, *slot, "clearScratch", view.scratch, 8192);
            if (skip) clear_buffer(enc,m4,*slot,"clearLossyPlane",view.lossyPlane,10240);
            if (view.zeroTargets || p.reset) {
                // Reset PRE reads current inputs only and fully overwrites
                // reprojected; POST initializes history and recurrent before
                // subsequent frames can sample them. Preserve initialization
                // for newly supplied targets when history is not reset.
                if (!p.reset) {
                    clear_texture(enc, m4, *slot, "clearHistory", view.history);
                    clear_texture(enc, m4, *slot, "clearReprojected", view.reprojected);
                    clear_texture(enc, m4, *slot, "clearRecurrent", view.recurrent);
                }
                helper(enc, m4, *slot, "initExposure", "fsr411_init_exposure", {}, {view.autoExposure}, MTLSizeMake(1,1,1));
            }
            if (p.copyMask & FSR4_INPUT_COPY_COLOR) {
                copy(enc, m4, *slot, "copyColor", frame.color, view.colorCopy, p.colorRegion, 0, 0, 8448, false);
                b.color = view.colorCopy;
            }
            if ((p.copyMask & FSR4_INPUT_COPY_DEPTH) || depth_format(frame.depth.pixelFormat)) {
                copy(enc, m4, *slot, "copyDepth", frame.depth, view.depthCopy, p.depthRegion, 0, 0, 8704, depth_format(frame.depth.pixelFormat));
                b.depth = view.depthCopy;
            }
            if (p.copyMask & FSR4_INPUT_COPY_MOTION) {
                copy(enc, m4, *slot, "copyMotion", frame.motion, view.motionCopy, p.motionRegion, 0, 0, 8960, false);
                b.motion = view.motionCopy;
            }
            if (slot->snapshot) {
                const Fsr4InputRect renderRegion{0,0,p.rw,p.rh}, motionRegion{0,0,p.mvW,p.mvH};
                copy(enc,m4,*slot,"snapshotColor",b.color,slot->snapshot->color,renderRegion,0,0,kSnapshot,false);
                copy(enc,m4,*slot,"snapshotDepth",b.depth,slot->snapshot->depth,renderRegion,0,0,kSnapshot+256,false);
                copy(enc,m4,*slot,"snapshotMotion",b.motion,slot->snapshot->motion,motionRegion,0,0,kSnapshot+512,false);
            }
            if (p.copyMask & FSR4_INPUT_COPY_OUTPUT) b.output = view.outputCopy;
            if (p.autoExposure) {
                if(!session->splitAutoExposure) {
                    clear_buffer(enc, m4, *slot, "clearAtomic", view.spdAtomic, 9216);
                    clear_texture(enc, m4, *slot, "clearLuma", view.luma);
                }
                dispatch(enc, m4, *slot, *session, "ae", MTLSizeMake(ceil_div(p.rw,64),ceil_div(p.rh,64),1), b);
                // dispatch() inserts a texture/buffer barrier on both encoder
                // APIs. The final group sees every tile without relying on
                // DirectX globallycoherent texture/atomic publication rules.
                if(session->splitAutoExposure && std::max(p.rw,p.rh)>=128)
                    dispatch(enc,m4,*slot,*session,"ae_finalize",MTLSizeMake(1,1,1),b);
            } else {
                const uint32_t reset = p.reset;
                put(*slot, 9472, reset);
                helper(enc, m4, *slot, "updateExposure", "fsr411_exposure",
                       {{slot->constants,9472}}, {b.exposure,view.autoExposure}, MTLSizeMake(1,1,1));
            }
            if (session->preparedColor)
                dispatch(enc,m4,*slot,*session,"prepare_color",MTLSizeMake(ceil_div(p.rw,16),ceil_div(p.rh,16),1),b);
            for (const auto &name : session->order) {
                if (skip && name!="pre" && name!="post") continue;
                if (name=="post" && session->externalPostCnb && !skip)
                    dispatch_native(enc,m4,*slot,*session,13,view,sizes);
                const int pass = name.rfind("pass",0) == 0 ? std::atoi(name.c_str()+4) : 0;
                auto sequence=session->nativeSequence.find(name);
                if(native_or_portable && sequence!=session->nativeSequence.end()) {
                    for(int id:sequence->second) dispatch_native(enc,m4,*slot,*session,id,view,sizes);
                } else if (native_or_portable && pass && session->native.count(pass)) {
                    dispatch_native(enc, m4, *slot, *session, pass, view, sizes);
                } else {
                    MTLSize groups;
                    if (name == "pre") groups = MTLSizeMake(ceil_div(p.alignedUW,16),ceil_div(p.alignedUH,16),1);
                    else if (name == "post") groups = MTLSizeMake(ceil_div(p.alignedUW,session->postPixelsPerGroup),ceil_div(p.alignedUH,session->postPixelsPerGroup),1);
                    else if (session->modelLite && name.rfind("pad",0)==0) {
                        const auto &k=session->kernels.at(name);
                        const unsigned shift=(k.tensorSlot==3 || k.tensorSlot==4)?2:1;
                        const uint32_t w=p.alignedUW>>shift,h=p.alignedUH>>shift;
                        const uint32_t c=std::min((uint32_t(tier*16/9)>>shift)+1u-w,5u);
                        const uint32_t d=std::min((uint32_t(tier)>>shift)+1u-h,1u);
                        const uint32_t f=w+1u+c;
                        groups=MTLSizeMake(ceil_div(f+h+h*c+f*d,32),1,1);
                    }
                    else if (name.rfind("pad",0) == 0) groups = MTLSizeMake(padding_groups(tier,std::atoi(name.c_str()+3),p.alignedUW,p.alignedUH),1,1);
                    else if(session->modelLite) {
                        const auto &k=session->kernels.at(name);
                        groups=MTLSizeMake(ceil_div(sizes.v[k.tensorSlot][0],k.tileX),ceil_div(sizes.v[k.tensorSlot][1],k.tileY),1);
                    }
                    else { const int shift = pass_shift(pass); groups = MTLSizeMake(ceil_div(p.alignedUW >> shift,64),p.alignedUH >> shift,1); }
                    // Older precompiled bundles use the shared PRE. New
                    // bundles specialize only the already-authorized reuse
                    // path, with its own bindings on both Metal APIs.
                    const std::string stage = skip && name=="pre" && session->kernels.count("pre_reuse")
                        ? "pre_reuse" : name;
                    dispatch(enc, m4, *slot, *session, stage, groups, b);
                }
                const auto capture = view.captures.find(name);
                if (capture != view.captures.end()) {
                    const uint32_t count = uint32_t(view.scratch.length / 16);
                    put(*slot,9728,count);
                    helper(enc,m4,*slot,"capture_"+name,"fsr411_snapshot",
                           {{view.scratch,0},{capture->second,0},{slot->constants,9728}}, {}, MTLSizeMake(count,1,1));
                }
            }
            if (rcas) dispatch(enc,m4,*slot,*session,"rcas",MTLSizeMake(ceil_div(p.alignedUW,16),ceil_div(p.alignedUH,16),1),b);
            if (p.copyMask & FSR4_INPUT_COPY_OUTPUT) {
                const Fsr4InputRect region{0,0,p.uw,p.uh};
                copy(enc,m4,*slot,"copyOutput",view.outputCopy,frame.output,region,p.outputRegion.x,p.outputRegion.y,9984,false);
            }
            if (slot->snapshot) {
                copy(enc,m4,*slot,"snapshotOutput",frame.output,slot->snapshot->output,p.outputRegion,0,0,kSnapshot+768,false);
                // Every snapshot texture write is visible before the standalone
                // token changes to one. Later frames never write these resources.
                helper(enc,m4,*slot,"snapshotDone","fsr411_retire",{{slot->snapshot->completion,0}},{},MTLSizeMake(1,1,1));
            }
            helper(enc,m4,*slot,"retire","fsr411_retire",{{slot->constants,FrameSlot::kDone}}, {}, MTLSizeMake(1,1,1));
            if (m4) queue_after(enc);
            [enc endEncoding];
        } @catch (NSException *exception) {
            // Partial GPU writes invalidate history. Keep the slot retained if
            // recording the retirement token itself failed; never recycle it.
            @try { [enc endEncoding]; } @catch (NSException *) {}
            return fail("FSR 4.1.1 encode: " + utf8(exception.reason));
        }
        view.zeroScratch = view.zeroTargets = false;
        fsr4_input_commit(&view.inputHistory,&p);
        view.prepared = p;
        view.variantKey = session->key;
        view.backendLabel = kernels_label(kind);
        view.postCnbAlias = session->postCnbAlias;
        view.scratchLayout = session->scratchLayout;
        view.cacheValid=session->externalPostCnb;
        view.skippedInference=skip;
        if(skip) ++view.reusedFrames;else ++view.neuralRuns;
        ++view.encodes;
        return true;
    }

private:
    struct BufferBinding { id<MTLBuffer> buffer; NSUInteger offset; };
    struct Bindings {
        id<MTLTexture> color, depth, motion, output, exposure;
        View *view; bool rcas;
    };
    static constexpr NSUInteger kTensor = 256, kSpd = 768, kRcas = 1024, kLossy = 1280, kNative = 2048;
    static constexpr NSUInteger kSnapshot = 12288;
    static_assert(kSnapshot + 768 + sizeof(RegionConstants) < FrameSlot::kDone, "snapshot constant region overflow");
    id<MTLDevice> device_;
    std::string root_;
    std::vector<std::string> initializerDirs_;
    std::map<std::string,std::unique_ptr<Session>> sessions_;
    std::map<std::string,id<MTLComputePipelineState>> helpers_;
    id<MTLSamplerState> sampler_ = nil;
    id<MTLTexture> exposureOne_ = nil;
    // Simd probe result per Runtime (one device): -1 unknown, 0 failed, 1 ok.
    int simdVerified_ = -1;
    bool simdFailed_ = false;
    // Winograd-range probe passed and FSR411_SIMD_WINOGRAD is not 0.
    bool winogradOk_ = false;

    bool native_capable() const {
        if (@available(macOS 26.0,*)) return [device_ supportsFamily:static_cast<MTLGPUFamily>(1010)]
            && device_.maxThreadgroupMemoryLength >= 20480 && device_.maxThreadsPerThreadgroup.width >= 128;
        return false;
    }
    bool simd_family() const { return [device_ supportsFamily:MTLGPUFamilyApple7]; }
    // Confirms the lane ownership the simd epilogue relies on and that half
    // products accumulate exactly in float on this GPU: INT8 extremes for the
    // direct kernels, then Winograd-range operands (|a| <= 512, |b| <= 2048,
    // sums near 2^24) for the transformed convolutions.
    bool verify_simd(id<MTLLibrary> lib, std::string &error) {
        if (simdVerified_ >= 0) { if (!simdVerified_) error = "simdgroup probe failed on this GPU"; return simdVerified_ == 1; }
        simdVerified_ = 0;
        NSError *e = nil;
        id<MTLFunction> fn = [lib newFunctionWithName:@"fsr411_simd_probe"];
        id<MTLComputePipelineState> pso = fn ? [device_ newComputePipelineStateWithFunction:fn error:&e] : nil;
        if (!pso || pso.maxTotalThreadsPerThreadgroup < 32 || pso.threadExecutionWidth != 32) {
            error = "simdgroup probe pipeline: " + (e ? metal_error(e) : std::string("needs SIMD32")); return false;
        }
        id<MTLCommandQueue> queue = [device_ newCommandQueue];
        auto probe = [&](bool wide, std::string &why) {
            _Float16 a[64], b[64]; int ai[64], bi[64]; uint32_t r = wide ? 0x5eedu : 0x4117u;
            for (int i = 0; i < 64; ++i) {
                r = r * 1664525u + 1013904223u;
                if (wide) {
                    // 2 * 8 * 511 * 2047 < 2^24 plus odd low terms in column 7.
                    ai[i] = i % 8 == 7 ? int(r >> 29) - 4 : (i % 2 ? -511 : 511);
                    bi[i] = i % 8 == 7 ? int(r >> 28) - 8 : (i / 8 % 2 ? -2047 : 2047);
                } else {
                    ai[i] = i % 3 == 0 ? -128 : i % 3 == 1 ? 127 : int(r >> 24) - 128;
                    bi[i] = i % 5 == 0 ? 127 : -128;
                }
                a[i] = (_Float16)ai[i]; b[i] = (_Float16)bi[i];
            }
            id<MTLBuffer> A = [device_ newBufferWithBytes:a length:sizeof a options:MTLResourceStorageModeShared];
            id<MTLBuffer> B = [device_ newBufferWithBytes:b length:sizeof b options:MTLResourceStorageModeShared];
            id<MTLBuffer> out = [device_ newBufferWithLength:96 * sizeof(int) options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso]; [enc setBuffer:A offset:0 atIndex:0];
            [enc setBuffer:B offset:0 atIndex:1]; [enc setBuffer:out offset:0 atIndex:2];
            [enc dispatchThreadgroups:MTLSizeMake(1,1,1) threadsPerThreadgroup:MTLSizeMake(32,1,1)];
            [enc endEncoding]; [cb commit]; [cb waitUntilCompleted];
            if (cb.status != MTLCommandBufferStatusCompleted) { why = "simdgroup probe did not complete"; return false; }
            const int *v = (const int *)out.contents;
            for (int i = 0; i < 8; ++i) for (int j = 0; j < 8; ++j) {
                int s = 0; for (int k = 0; k < 8; ++k) s += ai[i*8+k] * bi[k*8+j];
                if (v[i*8+j] != 2 * s) { why = "simdgroup half products do not accumulate exactly in float"; return false; }
            }
            for (int lane = 0; lane < 32; ++lane)
                if (v[64+lane] != 1) { why = "simdgroup element layout differs from the verified Apple7+ layout"; return false; }
            return true;
        };
        if (!probe(false, error)) return false;
        std::string why;
        const char *disable = getenv("FSR411_SIMD_WINOGRAD");
        winogradOk_ = !(disable && !std::strcmp(disable, "0")) && probe(true, why);
        if (!winogradOk_ && !why.empty()) fsr4_logf("FSR 4.1.1 simd Winograd disabled (%s); direct kernels", why.c_str());
        simdVerified_ = 1; return true;
    }
    static bool depth_format(MTLPixelFormat format) {
        return format == MTLPixelFormatDepth16Unorm || format == MTLPixelFormatDepth32Float
            || format == MTLPixelFormatDepth32Float_Stencil8;
    }
    bool validate_textures(const Frame &f, std::string &error) const {
        const id<MTLTexture> textures[] = {f.color,f.depth,f.motion,f.output};
        const Fsr4InputSurface surfaces[] = {f.input.geometry.color,f.input.geometry.depth,f.input.geometry.motion,f.input.geometry.output};
        for (int i=0;i<4;++i) {
            if (!textures[i] || textures[i].device != device_ || textures[i].textureType != MTLTextureType2D
                || textures[i].sampleCount != 1 || textures[i].width != surfaces[i].width || textures[i].height != surfaces[i].height) {
                error = "frame geometry does not match single-sample Metal textures"; return false;
            }
        }
        if (f.input.hasExposureTexture != (f.exposure != nil)) { error="exposure descriptor/resource mismatch"; return false; }
        if (f.exposure && (f.exposure.device != device_ || f.exposure.textureType != MTLTextureType2D
            || !f.exposure.width || !f.exposure.height || f.exposure.sampleCount != 1)) { error="invalid exposure texture"; return false; }
        return true;
    }
    id<MTLTexture> texture(MTLPixelFormat format, uint32_t w, uint32_t h, MTLStorageMode storage = MTLStorageModePrivate) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format width:w height:h mipmapped:NO];
        d.storageMode = storage;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
        return [device_ newTextureWithDescriptor:d];
    }
    bool prepare_snapshot(FrameSnapshot &snapshot, const Frame &frame, const Fsr4InputPrepared &p, std::string &error) {
        if (snapshot.completion || snapshot.color || snapshot.depth || snapshot.motion || snapshot.output) {
            error = "a frame snapshot can only be used for one encode"; return false;
        }
        auto color=texture(MTLPixelFormatRGBA32Float,p.rw,p.rh,MTLStorageModeShared);
        auto depth=texture(MTLPixelFormatR32Float,p.rw,p.rh,MTLStorageModeShared);
        auto motion=texture(MTLPixelFormatRG32Float,p.mvW,p.mvH,MTLStorageModeShared);
        auto output=texture(MTLPixelFormatRGBA32Float,p.uw,p.uh,MTLStorageModeShared);
        auto completion=[device_ newBufferWithLength:sizeof(uint32_t) options:MTLResourceStorageModeShared];
        if (!color || !depth || !motion || !output || !completion) {
            error = "cannot allocate CPU-readable frame snapshot"; return false;
        }
        __atomic_store_n((uint32_t *)completion.contents,0u,__ATOMIC_RELEASE);
        snapshot.input=frame.input; snapshot.prepared=p;
        snapshot.color=color; snapshot.depth=depth; snapshot.motion=motion; snapshot.output=output;
        snapshot.completion=completion;
        return true;
    }
    bool fit(id<MTLTexture> __strong &target, MTLPixelFormat format, uint32_t w, uint32_t h) {
        if (!target || target.width != w || target.height != h || target.pixelFormat != format) target = texture(format,w,h);
        return target != nil;
    }
    bool ensure_targets(View &v, const Session &s, const Fsr4InputPrepared &p, const Frame &f, const Options &o, std::string &error) {
        if (!v.scratch || v.scratch.length != s.scratchBytes) {
            v.scratch = [device_ newBufferWithLength:s.scratchBytes options:MTLResourceStorageModePrivate];
            v.zeroScratch = true;
        }
        if (v.width != p.alignedUW || v.height != p.alignedUH || !v.history || !v.reprojected || !v.recurrent) {
            v.history = texture(MTLPixelFormatRGBA16Float,p.alignedUW,p.alignedUH);
            v.reprojected = texture(MTLPixelFormatRGBA16Float,p.alignedUW,p.alignedUH);
            v.recurrent = texture(MTLPixelFormatRGBA8Unorm,p.alignedUW,p.alignedUH);
            v.width=p.alignedUW; v.height=p.alignedUH; v.zeroTargets=true;
        }
        if (!v.autoExposure) v.autoExposure=texture(MTLPixelFormatR32Float,2,1);
        bool ok = v.scratch && v.history && v.reprojected && v.recurrent && v.autoExposure;
        if (s.preparedColor) ok &= fit(v.preparedColor,MTLPixelFormatRGBA32Float,p.rw,p.rh);
        if(profile_is_lossy(o.profile)) {
            const size_t blocks=size_t(p.alignedUW/4u)*(p.alignedUH/4u);
            if(!v.lossyMotion || v.lossyMotion.length!=blocks*16u)
                v.lossyMotion=[device_ newBufferWithLength:blocks*16u options:MTLResourceStorageModePrivate];
            if(!v.lossyPlane || v.lossyPlane.length!=blocks*4u)
                v.lossyPlane=[device_ newBufferWithLength:blocks*4u options:MTLResourceStorageModePrivate];
            ok &= v.lossyMotion && v.lossyPlane;
        }
        // Float32 canonical copies preserve the values returned by the source
        // texture, including sRGB decode and packed R11G11B10 input precision.
        if (p.copyMask & FSR4_INPUT_COPY_COLOR) ok &= fit(v.colorCopy,MTLPixelFormatRGBA32Float,p.rw,p.rh);
        if ((p.copyMask & FSR4_INPUT_COPY_DEPTH) || depth_format(f.depth.pixelFormat)) ok &= fit(v.depthCopy,MTLPixelFormatR32Float,p.rw,p.rh);
        if (p.copyMask & FSR4_INPUT_COPY_MOTION) ok &= fit(v.motionCopy,MTLPixelFormatRG32Float,p.mvW,p.mvH);
        if (p.copyMask & FSR4_INPUT_COPY_OUTPUT) ok &= fit(v.outputCopy,MTLPixelFormatRGBA32Float,p.uw,p.uh);
        if (o.rcas && o.sharpness > 0) ok &= fit(v.rcasTemp,MTLPixelFormatRGBA16Float,p.alignedUW,p.alignedUH);
        if (p.autoExposure) {
            ok &= fit(v.luma,MTLPixelFormatR32Float,std::max(ceil_div(p.rw,64),1u),std::max(ceil_div(p.rh,64),1u));
            if (!s.splitAutoExposure && !v.spdAtomicTexture) {
                if (!v.spdAtomic) v.spdAtomic=[device_ newBufferWithLength:4096 options:MTLResourceStorageModeShared];
                const NSUInteger row=std::max<NSUInteger>(16,[device_ minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatR32Uint]);
                auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:1 height:1 mipmapped:NO];
                d.storageMode=MTLStorageModeShared; d.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;
                v.spdAtomicTexture=[v.spdAtomic newTextureWithDescriptor:d offset:0 bytesPerRow:row];
            }
            if(!s.splitAutoExposure) ok &= v.spdAtomic && v.spdAtomicTexture;
        }
        if (!ok) error="cannot allocate FSR 4.1.1 textures or scratch";
        return ok;
    }
    bool ensure_helpers(std::string &error) {
        if (!helpers_.empty()) return true;
        NSError *e=nil;
        // bbport: built with the runtime (build_assets.py), never compiled here.
        id<MTLLibrary> lib=[device_ newLibraryWithURL:[NSURL fileURLWithPath:ns(root_+"/fsr411_helpers.metallib")] error:&e];
        if (!lib) { error=metal_error(e); return false; }
        const char *names[]={"fsr411_copy","fsr411_copy_depth","fsr411_clear_texture","fsr411_clear_buffer",
                             "fsr411_snapshot","fsr411_exposure","fsr411_init_exposure","fsr411_retire"};
        std::map<std::string,id<MTLComputePipelineState>> next;
        for (auto name:names) {
            id<MTLFunction> fn=[lib newFunctionWithName:ns(name)];
            auto pso=[device_ newComputePipelineStateWithFunction:fn error:&e];
            if (!pso) { error=metal_error(e); return false; }
            next.emplace(name,pso);
        }
        MTLSamplerDescriptor *sd=[MTLSamplerDescriptor new];
        sd.minFilter=sd.magFilter=MTLSamplerMinMagFilterLinear;
        sd.sAddressMode=sd.tAddressMode=MTLSamplerAddressModeClampToEdge;
        sd.supportArgumentBuffers=YES;
        sampler_=[device_ newSamplerStateWithDescriptor:sd];
        auto d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Float width:1 height:1 mipmapped:NO];
        d.storageMode=MTLStorageModeShared; d.usage=MTLTextureUsageShaderRead;
        exposureOne_=[device_ newTextureWithDescriptor:d];
        float one=1;
        [exposureOne_ replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&one bytesPerRow:4];
        if (!sampler_ || !exposureOne_) { error="cannot allocate common FSR 4.1.1 resources"; return false; }
        helpers_=std::move(next);
        return true;
    }
    NSDictionary *json(const std::string &path, std::string &error) {
        NSError *e=nil; NSData *data=[NSData dataWithContentsOfFile:ns(path) options:0 error:&e];
        id result=data?[NSJSONSerialization JSONObjectWithData:data options:0 error:&e]:nil;
        if (![result isKindOfClass:[NSDictionary class]]) { error="invalid FSR 4.1.1 manifest: "+path+": "+metal_error(e); return nil; }
        return result;
    }
    bool load_native(Session &s, bool compile, Kernels kind, std::string &error) {
        (void)compile;
        const bool useMpp=kind==Kernels::Mpp, useSimd=kind==Kernels::Simd;
        // bbport: the built networks (build_assets.py), with weights assembled from the
        // player's initializer (assemble_weights); simd and M5 kernels only.
        if (!useMpp && !useSimd) { error="this build has only the simd and M5 FSR 4.1.x kernels"; return false; }
        if (s.profile!=Profile::ExactFast && s.profile!=Profile::Lossy2 && s.profile!=Profile::Int8Lite) {
            error="this build has only the FSR 4.1 (exact-fast), 4.1 Lossy and 4.1.2 Lite networks"; return false;
        }
        const std::string dir=root_+"/"+(s.modelLite?"fsr412_native_":"fsr411_native_")+std::to_string(s.tier)
            +(s.ultraPerformance?"_ultraperformance":"")
            +(s.profile==Profile::Lossy2?"_lossy2":"");
        NSData *manifestData=[NSData dataWithContentsOfFile:ns(dir+"/fsr411_native_manifest.json")];
        NSMutableDictionary *manifest=manifestData?[NSJSONSerialization JSONObjectWithData:manifestData
            options:NSJSONReadingMutableContainers error:nil]:nil;
        if (![manifest isKindOfClass:[NSMutableDictionary class]] || [manifest[@"abi_version"] unsignedIntValue]!=kAbiVersion
            || [manifest[@"parameter_size"] unsignedIntValue]!=sizeof(PassParams)) { error="native FSR manifest missing or ABI mismatch"; return false; }
        const std::string recipeName=s.modelLite?"fsr412_native_weights_recipe.bin":"fsr411_native_recipe.bin";
        NSData *recipe=[NSData dataWithContentsOfFile:ns(dir+"/"+recipeName)];
        NSData *weights=recipe?assemble_weights(manifest,recipe,
            [NSData dataWithBytesNoCopy:s.initializer.contents length:s.initializer.length freeWhenDone:NO],error):nil;
        if (!weights) { if(error.empty()) error="missing native FSR weight recipe"; return false; }
        NSData *params=[NSData dataWithContentsOfFile:ns(dir+"/fsr411_native_params.bin")];
        if (!weights.length || !params.length) { error="missing native weights/parameters"; return false; }
        if(weights.length != [manifest[@"weights_bytes"] unsignedLongLongValue]) {
            error="native 4.1.1 weight cache is incomplete; regenerate this model";return false;
        }
        if(manifest[@"params_bytes"] && params.length != [manifest[@"params_bytes"] unsignedLongLongValue]) {
            error="native 4.1.1 parameter cache is incomplete; regenerate this model";return false;
        }
        s.nativeWeights=[device_ newBufferWithBytes:weights.bytes length:weights.length options:MTLResourceStorageModeShared];
        if (useSimd) {
            // Same element indices as the INT8 blob; bias words become unused
            // halves (biases are read as int32 from nativeWeights).
            std::vector<_Float16> widened(weights.length);
            const int8_t *bytes=(const int8_t *)weights.bytes;
            for (size_t i=0;i<widened.size();++i) widened[i]=(_Float16)bytes[i];
            // Winograd regions hold int16 values at the same starting index.
            NSMutableArray *entries=[[manifest[@"passes"] allValues] mutableCopy];
            if (manifest[@"auxiliary_passes"][@"post_cnb"]) [entries addObject:manifest[@"auxiliary_passes"][@"post_cnb"]];
            for (NSDictionary *entry in entries) {
                NSDictionary *w=entry[@"simd"][@"winograd"]; if (!w) continue;
                const size_t at=[w[@"weights0"] unsignedLongLongValue], count=[w[@"weights0_count"] unsignedLongLongValue];
                if (at+2*count>weights.length) { error="native 4.1.1 Winograd weights out of range"; return false; }
                for (size_t i=0;i<count;++i) { int16_t v; memcpy(&v,bytes+at+2*i,2); widened[at+i]=(_Float16)v; }
            }
            s.halfWeights=[device_ newBufferWithBytes:widened.data() length:widened.size()*sizeof(_Float16) options:MTLResourceStorageModeShared];
            if (!s.halfWeights) { error="cannot allocate simd half weights"; return false; }
        }
        // bbport: the libraries built with the runtime (build_assets.py): simd for every tier,
        // M5's per tier (FSR411_MAXW/MAXH), both with FSR411_SPECIALIZE=1.
        NSError *e=nil;
        const bool specialize=useMpp||useSimd;
        const std::string library=useMpp?"/fsr411_mpp_"+std::to_string(s.tier)+".metallib":"/fsr411_simd.metallib";
        id<MTLLibrary> lib=[device_ newLibraryWithURL:[NSURL fileURLWithPath:ns(root_+library)] error:&e];
        if (!lib) { error=metal_error(e); return false; }
        if (useSimd && !verify_simd(lib,error)) return false;
        std::map<std::string,id<MTLComputePipelineState>> states;
        NSMutableDictionary *passes=[manifest[@"passes"] mutableCopy];
        if(s.externalPostCnb) {
            NSDictionary *post=manifest[@"auxiliary_passes"][@"post_cnb"];
            if(!post || ![post[@"output_offset_base"] isEqual:@"scratch_append"]) { error="missing native 4.1.1 POST CNB";return false; }
            if([post[@"scratch_append_bytes"] unsignedLongLongValue]!=s.scratchLayout.cacheBytes ||
               [post[@"scratch_append_alignment"] unsignedIntValue]!=256u ||
               ![post[@"kind"] isEqual:@"cnb"] || [post[@"input_slot"] unsignedIntValue]!=(s.modelLite?5u:13u) ||
               [post[@"output_slot"] unsignedIntValue]!=(s.modelLite?5u:13u) || post[@"skip_slot"] ||
               [post[@"before_stage"] unsignedIntValue]!=(s.modelLite?9u:26u)) {
                error="native 4.1.1 POST CNB layout mismatch";return false;
            }
            passes[@"13"]=post;
            s.scratchBytes=size_t(s.scratchLayout.allocationBytes);
        }
        for (NSString *key in passes) {
            NSDictionary *entry=passes[key]; NativePass np;
            NSDictionary *dispatch=useMpp?entry:useSimd?entry[@"simd"]:entry[@"reference"];
            if (!dispatch) { error="native 4.1.1 manifest predates the simd kernels; regenerate this model"; return false; }
            NSDictionary *wino=useSimd && winogradOk_ ? dispatch[@"winograd"] : nil;
            if (wino) dispatch=wino;
            const std::string name=utf8(useMpp?entry[@"kernel"]:dispatch[@"function"]);
            np.offset=[entry[@"parameter_offset"] unsignedIntValue];
            if (np.offset+sizeof(PassParams)>params.length || kNative+np.offset+sizeof(PassParams)>8192) { error="native parameter region overflow"; return false; }
            memcpy(&np.parameters,(const uint8_t *)params.bytes+np.offset,sizeof(PassParams));
            if([key intValue]==13 && !s.modelLite && !post_cnb_tensors_match_layout(np.parameters,s.scratchLayout)) {
                error="native 4.1.1 POST CNB tensors do not match the scratch layout";return false;
            }
            // Winograd reads its transformed 3x3 weights from the half copy.
            if (wino) np.parameters.weights[0]=[wino[@"weights0"] unsignedIntValue];
            // Shared entry names have different weights, strides and epilogues
            // for each logical pass. Dimensions remain per-frame parameters.
            const std::string pipelineKey=name+(specialize?"/"+utf8(key):"");
            if (!states.count(pipelineKey)) {
                id<MTLFunction> fn=nil;
                if (specialize) {
                    const auto metadata=specialize_metadata(np.parameters,[key intValue]==13);
                    MTLFunctionConstantValues *values=[MTLFunctionConstantValues new];
                    [values setConstantValues:metadata.values type:MTLDataTypeUInt4
                                    withRange:NSMakeRange(0,kPassSpecializationCount)];
                    fn=[lib newFunctionWithName:ns(name) constantValues:values error:&e];
                } else fn=[lib newFunctionWithName:ns(name)];
                id<MTLComputePipelineState> pso=nil;
                if (fn && s.modelLite) {
                    MTLComputePipelineDescriptor *descriptor=[MTLComputePipelineDescriptor new];
                    descriptor.computeFunction=fn;
                    descriptor.maxTotalThreadsPerThreadgroup=[dispatch[@"threads"] unsignedIntValue];
                    descriptor.threadGroupSizeIsMultipleOfThreadExecutionWidth=descriptor.maxTotalThreadsPerThreadgroup%32u==0;
                    pso=[device_ newComputePipelineStateWithDescriptor:descriptor options:MTLPipelineOptionNone reflection:nil error:&e];
                } else if (fn) pso=[device_ newComputePipelineStateWithFunction:fn error:&e];
                if (!pso) { error=metal_error(e); return false; }
                if (pso.maxTotalThreadsPerThreadgroup < [dispatch[@"threads"] unsignedIntValue]) {
                    error="pipeline "+name+" supports fewer threads than its tile needs"; return false;
                }
                states.emplace(pipelineKey,pso);
            }
            np.pso=states.at(pipelineKey);
            if ([key intValue]==13) np.parameters.output.offset+=uint32_t(s.scratchLayout.cacheOffset);
            np.inputSlot=[entry[@"input_slot"] unsignedIntValue]; np.outputSlot=[entry[@"output_slot"] unsignedIntValue];
            np.hasSkip=entry[@"skip_slot"]!=nil; np.skipSlot=[entry[@"skip_slot"] unsignedIntValue];
            np.tileX=[dispatch[@"tile"][0] unsignedIntValue]; np.tileY=[dispatch[@"tile"][1] unsignedIntValue];
            np.threads=[dispatch[@"threads"] unsignedIntValue]; np.inputDispatch=[entry[@"dispatch_space"] isEqual:@"input"];
            if (np.inputSlot>=17 || np.outputSlot>=17 || np.skipSlot>=17 || !np.tileX || !np.tileY || !np.threads) { error="invalid native dispatch metadata"; return false; }
            s.native.emplace([key intValue],np);
        }
        return s.nativeWeights && !s.native.empty();
    }
    #include "fsr412_session.inc"

    Session *ensure_session(int tier, const Fsr4InputPrepared &p, const Options &o, Kernels kind, std::string &error) {
        const bool portable=kind!=Kernels::Reference;
        const bool externalPost=portable || profile_is_lossy(o.profile);
        const std::string legacyKey="fsr411_"+std::to_string(tier)+"_di"+std::to_string(p.depthReversed)
            +"_lm"+std::to_string(p.lowResMV)+"_jm"+std::to_string(p.jitteredMV)+"_cs"+std::to_string(o.colorspace)
            +"_ae"+std::to_string(p.autoExposure)+"_o"+std::to_string(o.optimize)
            +(o.ultraPerformance?"_up1":"")+(o.profile==Profile::Lossy?"_lossy":o.profile==Profile::Lossy2?"_lossy2":o.profile==Profile::ExactFast?"_fast":"")
            +(externalPost?"_pc1":"")+(o.strictMath&&o.profile!=Profile::Exact?"_sm1":"");
        // Keep reference captures and existing no-compile bundles on their
        // appended-cache ABI. A separate key prevents mixing POST addresses.
        bool postCnbAlias=portable && o.optimize;
        const std::string suffix=kind==Kernels::Mpp?"_mpp":kind==Kernels::Simd?"_simd":kind==Kernels::Scalar?"_scalar":"_ref";
        if(postCnbAlias) {
            auto existing=sessions_.find(legacyKey+"_ca1"+suffix);
            if(existing!=sessions_.end()) return existing->second.get();
            {   // bbport: never compiled here, always the built bundles.
                // Pin an established fallback too: no disk polling or history
                // reset halfway through a stream when a cache build finishes.
                existing=sessions_.find(legacyKey+suffix);
                if(existing!=sessions_.end()) return existing->second.get();
                postCnbAlias=shader_bundle_files_ready(root_+"/"+legacyKey+"_ca1");
            }
        }
        const std::string key=legacyKey+(postCnbAlias?"_ca1":"");
        const std::string cacheKey=key+suffix;
        auto existing=sessions_.find(cacheKey); if(existing!=sessions_.end()) return existing->second.get();
        // bbport: the variants build_assets.py built (the ones the game uses).
        auto manifest=json(root_+"/"+key+"/bindings.json",error);
        if (!manifest || ![manifest[@"version"] isEqual:@"4.1.1"] || [manifest[@"order"] count]!=27) return nullptr;
        if([manifest[@"post_cnb_alias"] boolValue]!=postCnbAlias) {
            error="FSR 4.1.1 POST CNB shader key/layout mismatch";return nullptr;
        }
        auto s=std::make_unique<Session>(); s->key=cacheKey; s->tier=tier;
        s->postPixelsPerGroup=manifest[@"post_pixels_per_group"]?[manifest[@"post_pixels_per_group"] unsignedIntValue]:32;
        if(s->postPixelsPerGroup!=16 && s->postPixelsPerGroup!=32) {error="invalid POST dispatch geometry";return nullptr;}
        s->splitAutoExposure=[manifest[@"auto_exposure_dispatches"] unsignedIntValue]==2;
        s->ultraPerformance=o.ultraPerformance;s->externalPostCnb=externalPost;s->profile=o.profile;
        s->postCnbAlias=postCnbAlias;
        s->scratchBytes=[manifest[@"scratch_bytes"] unsignedLongLongValue];
        s->scratchLayout.arenaBytes=s->scratchLayout.allocationBytes=s->scratchBytes;
        if(externalPost) {
            if(!post_cnb_scratch_layout(uint32_t(tier),postCnbAlias,s->scratchLayout) ||
               s->scratchLayout.arenaBytes!=s->scratchBytes) {
                error="FSR 4.1.1 original scratch arena mismatch";return nullptr;
            }
            NSDictionary *layout=manifest[@"post_cnb_layout"];
            // Old append bundles did not carry layout metadata. New alias
            // bundles must state every byte range used by the host and POST.
            if(postCnbAlias && !layout) {error="missing POST CNB alias layout";return nullptr;}
            if(layout && (![layout isKindOfClass:[NSDictionary class]] ||
                ![layout[@"policy"] isEqual:postCnbAlias?@"alias_after_pass12_v1":@"append_v1"] ||
                [layout[@"arena_bytes"] unsignedLongLongValue]!=s->scratchLayout.arenaBytes ||
                [layout[@"source_plane_bytes"] unsignedLongLongValue]!=s->scratchLayout.sourcePlaneBytes ||
                [layout[@"offset"] unsignedLongLongValue]!=s->scratchLayout.cacheOffset ||
                [layout[@"bytes"] unsignedLongLongValue]!=s->scratchLayout.cacheBytes ||
                [layout[@"row_stride"] unsignedLongLongValue]!=s->scratchLayout.rowStride ||
                [layout[@"allocated_bytes"] unsignedLongLongValue]!=s->scratchLayout.allocationBytes ||
                ![layout[@"valid_after"] isEqual:@"post_cnb"] ||
                !layout[@"preserves_neural_intermediates_after_post"] ||
                [layout[@"preserves_neural_intermediates_after_post"] boolValue]==postCnbAlias)) {
                error="FSR 4.1.1 POST CNB layout metadata mismatch";return nullptr;
            }
        }
        // bbport: AMD's initializer from the player's folders, never shipped.
        NSData *init=find_initializer(root_,initializerDirs_,o.ultraPerformance,false,error);
        if (!init) return nullptr;
        if (!s->scratchBytes) { error="invalid FSR 4.1.1 bindings (no scratch)"; return nullptr; }
        s->initializer=[device_ newBufferWithBytes:init.bytes length:init.length options:MTLResourceStorageModeShared];
        if (!s->initializer) { error="cannot allocate FSR 4.1.1 initializer"; return nullptr; }
        for (NSString *stage in manifest[@"order"]) s->order.push_back(utf8(stage));
        if(postCnbAlias) for(size_t index=0;index<s->order.size();++index) {
            const std::string expected=index==0?"pre":index==26?"post":
                std::string(index&1?"pad":"pass")+std::to_string(index/2);
            if(s->order[index]!=expected) {error="POST CNB alias requires the original graph order";return nullptr;}
        }
        if (externalPost && !load_native(*s,false,kind,error)) return nullptr;
        NSDictionary *entries=manifest[@"kernels"];
        for (NSString *name in entries) {
            NSDictionary *entry=entries[name]; NSError *e=nil;
            // Native stages never execute the generated scalar shader. Avoid
            // compiling unused PSOs (and their large constant-folding graphs).
            const std::string stage=utf8(name);
            const int logical=stage.rfind("pass",0)==0?std::atoi(stage.c_str()+4):0;
            if(portable && logical && s->native.count(logical)) {s->kernels.emplace(stage,Kernel{});continue;}
            auto url=[NSURL fileURLWithPath:ns(root_+"/"+key+"/"+utf8(entry[@"library"]))];
            auto lib=[device_ newLibraryWithURL:url error:&e];
            if (!lib) { error=metal_error(e); return nullptr; }
            auto fn=[lib newFunctionWithName:entry[@"entry"]];
            auto pso=fn?[device_ newComputePipelineStateWithFunction:fn error:&e]:nil;
            if (!pso) { error="pipeline "+utf8(name)+": "+metal_error(e); return nullptr; }
            Kernel k; k.pso=pso; NSArray *wg=entry[@"wgsize"];
            if(NSString *tables=entry[@"literal_tables"]) {
                NSData *bytes=[NSData dataWithContentsOfFile:ns(root_+"/"+key+"/"+utf8(tables))];
                if(!bytes.length) {error="missing shader literal tables for "+stage;return nullptr;}
                k.literalTables=[device_ newBufferWithBytes:bytes.bytes length:bytes.length options:MTLResourceStorageModeShared];
                if(!k.literalTables) {error="cannot allocate shader literal tables";return nullptr;}
            }
            k.threads=MTLSizeMake([wg[0] unsignedIntValue],[wg[1] unsignedIntValue],[wg[2] unsignedIntValue]);
            for (NSArray *a in entry[@"buffers"]) k.buffers.emplace_back([a[0] unsignedIntegerValue],utf8(a[1]));
            for (NSArray *a in entry[@"textures"]) k.textures.emplace_back([a[0] unsignedIntegerValue],utf8(a[1]));
            for (NSArray *a in entry[@"samplers"]) k.samplers.emplace_back([a[0] unsignedIntegerValue],utf8(a[1]));
            s->kernels.emplace(utf8(name),std::move(k));
        }
        for (const auto &name:s->order) if(!s->kernels.count(name)) { error="incomplete 4.1.1 graph: "+name; return nullptr; }
        if (!s->kernels.count("ae") || !s->kernels.count("rcas")) { error="missing 4.1.1 AE/RCAS kernels"; return nullptr; }
        if(s->splitAutoExposure && !s->kernels.count("ae_finalize")) {error="missing 4.1.1 AE final reduction";return nullptr;}
        auto result=s.get(); sessions_.emplace(cacheKey,std::move(s)); return result;
    }
    FrameSlot *acquire_slot(View &v, std::string &error) {
        for (auto &slot:v.slots) if(slot->retired()) {
            slot->busy=false; [slot->resources removeAllObjects];
            slot->snapshot.reset();
            slot->stageProfiler=nullptr;
            if (slot->residency) { [slot->residency removeAllAllocations]; [slot->residency commit]; }
            return slot.get();
        }
        if (v.slots.size()>=32) { error="32 FSR frames are still in flight; wait for their command buffers"; return nullptr; }
        auto slot=std::make_unique<FrameSlot>();
        slot->constants=[device_ newBufferWithLength:FrameSlot::kSize options:MTLResourceStorageModeShared];
        slot->tables=[NSMutableDictionary new]; slot->resources=[NSMutableArray new];
        if (!slot->constants) { error="cannot allocate per-frame FSR constants"; return nullptr; }
        memset(slot->constants.contents,0,FrameSlot::kSize);
        auto result=slot.get(); v.slots.push_back(std::move(slot)); return result;
    }
    template <typename T> static void put(FrameSlot &slot, NSUInteger offset, const T &value) {
        memcpy((uint8_t *)slot.constants.contents+offset,&value,sizeof(T));
    }
    void fill_constants(FrameSlot &slot, const Session &s, const Fsr4InputPrepared &p, const TensorSizes &sizes, bool rcas, float sharpness, float pixelFixFloor = 0.f) {
        FrameConstants c={};
        c.inv_size[0]=1.f/p.alignedUW; c.inv_size[1]=1.f/p.alignedUH;
        c.scale[0]=float(p.alignedUW)/p.rw; c.scale[1]=float(p.alignedUH)/p.rh;
        c.inv_scale[0]=float(p.rw)/p.alignedUW; c.inv_scale[1]=float(p.rh)/p.alignedUH;
        c.tex_size[0]=p.alignedUW; c.tex_size[1]=p.alignedUH;
        c.max_renderSize[0]=p.maxInputWidth; c.max_renderSize[1]=p.maxInputHeight;
        memcpy(c.jitter,p.jitter,sizeof c.jitter); memcpy(c.mv_scale,p.mvScale,sizeof c.mv_scale);
        memcpy(c.jitterCancellation,p.jitterCancellation,sizeof c.jitterCancellation);
        c.width=p.alignedUW; c.height=p.alignedUH; c.width_lr=p.rw; c.height_lr=p.rh;
        c.reset=p.reset; c.preExposure=p.preExposure; c.previousPreExposure=p.previousPreExposure;
        c.rcas=rcas; c.sharpness=sharpness; c.pad=pixelFixFloor; put(slot,0,c); put(slot,kTensor,sizes);
        SpdConstants ae={}; ae.mips=std::clamp(uint32_t(std::floor(std::log2(std::max(p.rw,p.rh)))),1u,12u);
        ae.groups=ceil_div(p.rw,64)*ceil_div(p.rh,64); ae.invInputSize[0]=1.f/p.rw; ae.invInputSize[1]=1.f/p.rh;
        ae.preExposure=p.preExposure; ae.validSize[0]=p.rw; ae.validSize[1]=p.rh; put(slot,kSpd,ae);
        RcasConstants rc={}; const float value=std::exp2(2.f*sharpness-2.f);
        memcpy(&rc.config[0],&value,sizeof value); const _Float16 half=(_Float16)value; uint16_t h;
        memcpy(&h,&half,sizeof h); rc.config[1]=uint32_t(h)|(uint32_t(h)<<16); rc.preExposure=p.preExposure; put(slot,kRcas,rc);
        for (const auto &item:s.native) {
            const auto &n=item.second; auto param=n.parameters;
            param.input.width=sizes.v[n.inputSlot][0]; param.input.height=sizes.v[n.inputSlot][1];
            param.output.width=sizes.v[n.outputSlot][0]; param.output.height=sizes.v[n.outputSlot][1];
            if (n.hasSkip) { param.skip.width=sizes.v[n.skipSlot][0]; param.skip.height=sizes.v[n.skipSlot][1]; }
            put(slot,kNative+n.offset,param);
        }
    }
    bool table(FrameSlot &slot, const std::string &name, std::string &error) {
        if(slot.tables[ns(name)]) return true;
        if (@available(macOS 26.0,*)) {
            MTL4ArgumentTableDescriptor *d=[MTL4ArgumentTableDescriptor new];
            d.maxBufferBindCount=8; d.maxTextureBindCount=16; d.maxSamplerStateBindCount=4; d.initializeBindings=YES;
            NSError *e=nil; auto t=[device_ newArgumentTableWithDescriptor:d error:&e];
            if (!t) { error=metal_error(e); return false; }
            slot.tables[ns(name)]=t; return true;
        }
        error="Metal 4 argument tables unavailable"; return false;
    }
    bool prepare_tables(FrameSlot &slot, const Session &s, const View &v, std::string &error) {
        for (const auto &k:s.kernels) if(!table(slot,k.first,error)) return false;
        for (const auto &n:s.native) if(!table(slot,"native"+std::to_string(n.first),error)) return false;
        const char *names[]={"clearScratch","clearHistory","clearReprojected","clearRecurrent","initExposure","copyColor",
            "copyDepth","copyMotion","copyOutput","clearAtomic","clearLuma","clearLossyPlane","updateExposure","retire"};
        for (auto n:names) if(!table(slot,n,error)) return false;
        if (slot.snapshot) {
            const char *snapshotNames[]={"snapshotColor","snapshotDepth","snapshotMotion","snapshotOutput","snapshotDone"};
            for (auto n:snapshotNames) if(!table(slot,n,error)) return false;
        }
        for (const auto &c:v.captures) if(!table(slot,"capture_"+c.first,error)) return false;
        return true;
    }
    void retain_resources(FrameSlot &slot,const Session &s,const View &v,const Frame &f) {
        // The owner may resize on the next CPU encode while this frame is on
        // the GPU. Keep the old resources alive until its retirement token.
        const id resources[]={slot.constants,s.initializer,s.nativeWeights,s.halfWeights,v.history,v.reprojected,v.recurrent,v.autoExposure,
            v.scratch,v.spdAtomic,v.spdAtomicTexture,v.luma,v.colorCopy,v.depthCopy,v.motionCopy,v.outputCopy,v.rcasTemp,v.lossyMotion,v.lossyPlane,
            f.color,f.depth,f.motion,f.output,f.exposure,exposureOne_,v.preparedColor};
        for (id resource:resources) if(resource && ![slot.resources containsObject:resource]) [slot.resources addObject:resource];
        for(const auto &k:s.kernels) if(k.second.literalTables) [slot.resources addObject:k.second.literalTables];
        for (const auto &c:v.captures) [slot.resources addObject:c.second];
        if (slot.snapshot) {
            const auto &snapshot=*slot.snapshot;
            [slot.resources addObjectsFromArray:@[snapshot.color,snapshot.depth,snapshot.motion,snapshot.output,snapshot.completion]];
        }
    }
    bool bind_residency(FrameSlot &slot,id cb,std::string &error) {
        if (@available(macOS 26.0,*)) {
            if (!slot.residency) {
                NSError *e=nil; slot.residency=[device_ newResidencySetWithDescriptor:[MTLResidencySetDescriptor new] error:&e];
                if (!slot.residency) { error=metal_error(e); return false; }
            }
            for (id resource in slot.resources) [slot.residency addAllocation:(id<MTLAllocation>)resource];
            [slot.residency commit];
            if (![cb respondsToSelector:@selector(useResidencySet:)]) { error="Metal 4 command buffer lacks residency API"; return false; }
            [cb useResidencySet:slot.residency]; return true;
        }
        error="Metal 4 residency unavailable"; return false;
    }
    static void barrier(id enc,bool m4) {
        if (m4) [enc barrierAfterEncoderStages:MTLStageDispatch|MTLStageBlit beforeEncoderStages:MTLStageDispatch|MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
        else [(id<MTLComputeCommandEncoder>)enc memoryBarrierWithScope:MTLBarrierScopeBuffers|MTLBarrierScopeTextures];
    }
    static void queue_before(id enc) {
        [enc barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageDispatch|MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
    }
    static void queue_after(id enc) {
        [enc barrierAfterStages:MTLStageDispatch|MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
    }
    void helper(id __strong &enc,bool m4,FrameSlot &slot,const std::string &tag,const std::string &name,
                std::initializer_list<BufferBinding> buffers,std::initializer_list<id<MTLTexture>> textures,MTLSize grid) {
        if(slot.stageProfiler) slot.stageProfiler->begin_stage(enc,tag);
        auto pso=helpers_.at(name); NSUInteger index=0;
        if (m4) {
            id<MTL4ArgumentTable> t=slot.tables[ns(tag)];
            for(auto b:buffers) [t setAddress:b.buffer.gpuAddress+b.offset atIndex:index++];
            index=0; for(auto tx:textures) [t setTexture:tx.gpuResourceID atIndex:index++];
            [enc setArgumentTable:t];
        } else {
            auto ce=(id<MTLComputeCommandEncoder>)enc;
            for(auto b:buffers) [ce setBuffer:b.buffer offset:b.offset atIndex:index++];
            index=0; for(auto tx:textures) [ce setTexture:tx atIndex:index++];
        }
        [enc setComputePipelineState:pso];
        MTLSize threads=grid.height>1?MTLSizeMake(8,8,1):MTLSizeMake(std::min<NSUInteger>(256,grid.width),1,1);
        [enc dispatchThreads:grid threadsPerThreadgroup:threads]; barrier(enc,m4);
        if(slot.stageProfiler) slot.stageProfiler->end_stage(enc);
    }
    void clear_texture(id __strong &enc,bool m4,FrameSlot &s,const std::string &tag,id<MTLTexture> t) {
        helper(enc,m4,s,tag,"fsr411_clear_texture",{},{t},MTLSizeMake(t.width,t.height,1));
    }
    void clear_buffer(id __strong &enc,bool m4,FrameSlot &s,const std::string &tag,id<MTLBuffer> b,NSUInteger off) {
        uint32_t count=uint32_t(b.length/16); put(s,off,count);
        helper(enc,m4,s,tag,"fsr411_clear_buffer",{{b,0},{s.constants,off}},{},MTLSizeMake(count,1,1));
    }
    void copy(id __strong &enc,bool m4,FrameSlot &s,const std::string &tag,id<MTLTexture> from,id<MTLTexture> to,
              Fsr4InputRect rect,uint32_t x,uint32_t y,NSUInteger off,bool depth) {
        RegionConstants r={{rect.x,rect.y,rect.width,rect.height},{x,y,0,0}}; put(s,off,r);
        helper(enc,m4,s,tag,depth?"fsr411_copy_depth":"fsr411_copy",{{s.constants,off}},{from,to},MTLSizeMake(rect.width,rect.height,1));
    }
    id<MTLTexture> bind_texture(const std::string &name,const Bindings &b) {
        if(name=="r_input_color") return b.color;
        if(name=="r_prepared_color" || name=="rw_prepared_color") return b.view->preparedColor;
        if(name=="r_depth") return b.depth;
        if(name=="r_velocity") return b.motion;
        if(name=="r_input_exposure") return b.exposure;
        if(name=="r_auto_exposure_texture" || name=="rw_auto_exposure_texture") return b.view->autoExposure;
        if(name=="rw_spd_global_atomic") return b.view->spdAtomicTexture;
        if(name=="rw_autoexp_mip_5") return b.view->luma;
        if(name=="r_recurrent_0" || name=="rw_recurrent_0") return b.view->recurrent;
        if(name=="r_history_color" || name=="rw_history_color") return b.view->history;
        if(name=="r_reprojected_color" || name=="rw_reprojected_color") return b.view->reprojected;
        if(name=="rw_mlsr_output_color") return b.rcas?b.view->rcasTemp:b.output;
        if(name=="r_rcas_input") return b.view->rcasTemp;
        if(name=="rw_rcas_output") return b.output;
        [NSException raise:@"FSR411Binding" format:@"Unknown texture %s",name.c_str()]; return nil;
    }
    BufferBinding bind_buffer(const std::string &stage,const std::string &name,FrameSlot &slot,const Session &s,const Bindings &b) {
        if(name=="ScratchBuffer") return {b.view->scratch,0};
        if(name=="Fsr411Tables") return {s.kernels.at(stage).literalTables,0};
        if(name=="Fsr411LossyConstants") return {slot.constants,kLossy};
        if(name=="Fsr411LossyMotion") return {b.view->lossyMotion,0};
        if(name=="Fsr411LossyPlane") return {b.view->lossyPlane,0};
        if(name=="InitializerBuffer" || name=="t18_initializer") return {s.initializer,0};
        if(name=="MLSR_Optimized_Constants") return {slot.constants,(stage=="pre" || stage=="pre_reuse" || stage=="post" || stage=="prepare_color")?0:kTensor};
        if(name=="CsTensorSizes") return {slot.constants,kTensor};
        if(name=="AutoExposureSPDConstants") return {slot.constants,kSpd};
        if(name=="cbRCAS") return {slot.constants,kRcas};
        if(name=="rw_spd_global_atomic_atomic") return {b.view->spdAtomic,0};
        [NSException raise:@"FSR411Binding" format:@"Unknown buffer %s",name.c_str()]; return {nil,0};
    }
    void dispatch(id __strong &enc,bool m4,FrameSlot &slot,const Session &s,const std::string &name,MTLSize groups,const Bindings &b) {
        if(slot.stageProfiler) slot.stageProfiler->begin_stage(enc,name=="pre_reuse"?"pre":name);
        const auto &k=s.kernels.at(name);
        if(m4) {
            id<MTL4ArgumentTable> t=slot.tables[ns(name)];
            for(const auto &bb:k.buffers) {auto binding=bind_buffer(name,bb.second,slot,s,b); [t setAddress:binding.buffer.gpuAddress+binding.offset atIndex:bb.first];}
            for(const auto &tx:k.textures) [t setTexture:bind_texture(tx.second,b).gpuResourceID atIndex:tx.first];
            for(const auto &sm:k.samplers) [t setSamplerState:sampler_.gpuResourceID atIndex:sm.first];
            [enc setArgumentTable:t];
        } else {
            auto ce=(id<MTLComputeCommandEncoder>)enc;
            for(const auto &bb:k.buffers) {auto binding=bind_buffer(name,bb.second,slot,s,b); [ce setBuffer:binding.buffer offset:binding.offset atIndex:bb.first];}
            for(const auto &tx:k.textures) [ce setTexture:bind_texture(tx.second,b) atIndex:tx.first];
            for(const auto &sm:k.samplers) [ce setSamplerState:sampler_ atIndex:sm.first];
        }
        [enc setComputePipelineState:k.pso]; [enc dispatchThreadgroups:groups threadsPerThreadgroup:k.threads]; barrier(enc,m4);
        if(slot.stageProfiler) slot.stageProfiler->end_stage(enc);
    }
    void dispatch_native(id __strong &enc,bool m4,FrameSlot &slot,const Session &s,int pass,const View &v,const TensorSizes &sizes) {
        if(slot.stageProfiler) slot.stageProfiler->begin_stage(enc,pass==13?"post_cnb":"pass"+std::to_string(pass));
        const auto &n=s.native.at(pass); const unsigned shape=n.inputDispatch?n.inputSlot:n.outputSlot;
        if(m4) {
            id<MTL4ArgumentTable> t=slot.tables[ns("native"+std::to_string(pass))];
            [t setAddress:v.scratch.gpuAddress atIndex:0]; [t setAddress:s.nativeWeights.gpuAddress atIndex:1];
            [t setAddress:slot.constants.gpuAddress+kNative+n.offset atIndex:2];
            if (s.halfWeights) [t setAddress:s.halfWeights.gpuAddress atIndex:3];
            [enc setArgumentTable:t];
        } else {
            auto ce=(id<MTLComputeCommandEncoder>)enc;
            [ce setBuffer:v.scratch offset:0 atIndex:0]; [ce setBuffer:s.nativeWeights offset:0 atIndex:1];
            [ce setBuffer:slot.constants offset:kNative+n.offset atIndex:2];
            if (s.halfWeights) [ce setBuffer:s.halfWeights offset:0 atIndex:3];
        }
        [enc setComputePipelineState:n.pso];
        [enc dispatchThreadgroups:MTLSizeMake(ceil_div(sizes.v[shape][0],n.tileX),ceil_div(sizes.v[shape][1],n.tileY),1)
            threadsPerThreadgroup:MTLSizeMake(n.threads,1,1)]; barrier(enc,m4);
        if(slot.stageProfiler) slot.stageProfiler->end_stage(enc);
    }
};
} // namespace fsr411

/* Metal GPU backend for the shared Brotato core.
 *
 * There is ONE simulation: this file contains no step/reset/observe logic. It
 * compiles brotato_core.h - the same header the CPU env and (later) the CUDA
 * backend use - into two places:
 *
 *   1. the host translation unit, for sizeof(BtState) and for host-side
 *      accounting (nothing else: the state is initialised on the GPU);
 *   2. the Metal library source, by composing a source string that
 *      #includes the core header by path and then defines the kernel.
 *
 * `[device newLibraryWithSource:]` has no include-search-path option, so the
 * core header is located on disk (see brotato_metal_core_header) and included
 * by absolute path. Metal resolves the core header's own relative includes
 * (brotato_state.h, brotato_rng.h, the generated numeric tables) relative to
 * that file, so only one path has to be resolved. Set BROTATO_CORE_DIR to
 * override the search when the sources are not next to the binary.
 */

#include "brotato_metal.h"

/* Host-side view: BtState (via brotato_metal.h -> brotato_state.h) and the
 * .h's BT_OBS_SIZE / BT_ACTIONS constants.
 *
 * brotato_core.h is deliberately NOT included here. Its five public
 * bt_core_* entry points are non-static definitions, and this backend never
 * calls them on the host, so including it would put a second host definition of
 * those symbols into the link alongside the CPU env / parity test. The kernel
 * below gets the full core through the MSL translation unit instead. */

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <limits.h>
#include <mach-o/dyld.h>
#include <stdlib.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------------- */
/* Locating brotato_core.h                                                   */
/* ------------------------------------------------------------------------- */

static bool bt_metal_is_file(const std::string &path) {
    struct stat st;
    if (path.empty()) return false;
    if (stat(path.c_str(), &st) != 0) return false;
    return S_ISREG(st.st_mode);
}

static std::string bt_metal_dirname(const std::string &path) {
    size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) return std::string(".");
    if (slash == 0) return std::string("/");
    return path.substr(0, slash);
}

static std::string bt_metal_executable_dir() {
    char buf[PATH_MAX];
    uint32_t size = (uint32_t)sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) return std::string();
    char resolved[PATH_MAX];
    if (!realpath(buf, resolved)) return std::string();
    return bt_metal_dirname(resolved);
}

/* Absolute path of the core header, resolved once. Throws with every candidate
 * it tried when the header cannot be found, instead of silently compiling a
 * library without the simulation. */
static const std::string &brotato_metal_core_header() {
    static std::string cached;
    if (!cached.empty()) return cached;

    std::vector<std::string> candidates;
    const char *override_dir = getenv("BROTATO_CORE_DIR");
    if (override_dir && *override_dir) {
        std::string d(override_dir);
        if (!d.empty() && d[d.size() - 1] == '/') d.erase(d.size() - 1);
        candidates.push_back(d + "/brotato_core.h");
    }
    std::string self_dir = bt_metal_dirname(__FILE__);
    candidates.push_back(self_dir + "/brotato_core.h");
    candidates.push_back(self_dir + "/../ocean/brotato/brotato_core.h");
    candidates.push_back(std::string("ocean/brotato/brotato_core.h"));
    candidates.push_back(std::string("../ocean/brotato/brotato_core.h"));
    std::string exe_dir = bt_metal_executable_dir();
    if (!exe_dir.empty()) {
        candidates.push_back(exe_dir + "/ocean/brotato/brotato_core.h");
        candidates.push_back(exe_dir + "/../ocean/brotato/brotato_core.h");
    }

    for (size_t i = 0; i < candidates.size(); ++i) {
        if (!bt_metal_is_file(candidates[i])) continue;
        char resolved[PATH_MAX];
        if (!realpath(candidates[i].c_str(), resolved)) continue;
        cached = resolved;
        return cached;
    }
    std::string message = "Brotato Metal: cannot locate brotato_core.h; tried:";
    for (size_t i = 0; i < candidates.size(); ++i) message += "\n  " + candidates[i];
    message += "\nSet BROTATO_CORE_DIR to the directory containing brotato_core.h.";
    throw std::runtime_error(message);
}

static std::string bt_metal_quote_include_path(const std::string &path) {
    if (path.find('"') != std::string::npos || path.find('\\') != std::string::npos)
        throw std::runtime_error("Brotato Metal: core header path contains a quote or backslash");
    return path;
}

/* ------------------------------------------------------------------------- */
/* Include-closure fingerprint                                               */
/* ------------------------------------------------------------------------- */

/* Metal's runtime compiler caches the compiled library. The source string this
 * file hands to newLibraryWithSource: is only `#include "<abs path>"` + the
 * kernel, so the *header contents* are not part of what the compiler sees as
 * its source: a header edit would otherwise be served from the cache and the
 * kernel would keep running against a stale BtState layout / stale inlined
 * code while the host uses the new one. Fingerprinting the whole local include
 * closure and embedding it in the source makes a header change a cache miss.
 * (A stale layout is silent and nasty: obs/rewards/terminals live in their own
 * buffers and stay correct, so only the strided BtState reads - the numeric
 * log - come out as garbage.) */
static unsigned long long bt_metal_fnv1a(const void *data, size_t n, unsigned long long h) {
    const unsigned char *p = (const unsigned char *)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

static bool bt_metal_read_file(const std::string &path, std::string *out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t n;
    out->clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out->append(buf, n);
    fclose(f);
    return true;
}

static void bt_metal_hash_closure(const std::string &path, unsigned long long *h, int depth) {
    if (depth > 16) return;
    std::string text;
    if (!bt_metal_read_file(path, &text)) return;
    *h = bt_metal_fnv1a(text.data(), text.size(), *h);
    size_t pos = 0;
    while ((pos = text.find("#include \"", pos)) != std::string::npos) {
        size_t const hash_start = pos + 10;
        size_t end = text.find('"', hash_start);
        if (end == std::string::npos) break;
        std::string inc = text.substr(hash_start, end - hash_start);
        pos = end + 1;
        if (inc.empty() || inc[0] == '<' || inc[0] == '/') continue;   /* system/absolute: handled below */
        bt_metal_hash_closure(bt_metal_dirname(path) + "/" + inc, h, depth + 1);
    }
}

static const char *bt_metal_closure_hash_hex() {
    static char buf[32];
    static bool done = false;
    if (!done) {
        unsigned long long h = 1469598103934665603ull;
        bt_metal_hash_closure(brotato_metal_core_header(), &h, 0);
        std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
        done = true;
    }
    return buf;
}

/* ------------------------------------------------------------------------- */
/* Metal Shading Language source                                             */
/* ------------------------------------------------------------------------- */

/* Mirrors struct BrotatoMetalConfig in brotato_metal.h: `bt_u64` is the core's
 * portability typedef for the 64-bit seed (MSL rejects `unsigned long long`). */
static const char *BROTATO_METAL_KERNEL_SOURCE =
    "struct BrotatoMetalConfig { bt_u64 seed; int zone; int shop_steps; int character; int starting_weapon; int endless; int audio_rng; int particles_rng; int invulnerable; };\n"
    "struct BrotatoMetalDispatch { int start; int count; };\n"
    "\n"
    "/* One entry point per phase instead of one kernel branching on a runtime\n"
    " * mode. The Metal backend compiler (AGXCompilerCore) optimizes each entry\n"
    " * function separately, so the three phases are never merged into a single\n"
    " * enormous function that the backend's per-function passes cannot handle.\n"
    " * brotato.cu splits the same core the same way\n"
    " * (bt_gpu_init_kernel / bt_gpu_reset_kernel / bt_gpu_step_kernel). Each\n"
    " * kernel declares only the buffers its phase reads and writes. */\n"
    "kernel void brotato_metal_init_kernel(\n"
    "    device BtState* states [[buffer(0)]],\n"
    "    device float* obs [[buffer(1)]],\n"
    "    device float* rewards [[buffer(2)]],\n"
    "    device float* terminals [[buffer(3)]],\n"
    "    constant BrotatoMetalConfig& cfg [[buffer(4)]],\n"
    "    constant BrotatoMetalDispatch& args [[buffer(5)]],\n"
    "    uint gid [[thread_position_in_grid]])\n"
    "{\n"
    "    if (gid >= uint(args.count)) return;\n"
    "    int idx = args.start + int(gid);\n"
    "    device BtState* s = &states[idx];\n"
    "    /* Mirror the CPU env's seed input exactly: the harness writes\n"
    "     * Env.rng (default = env index) and puf_init applies\n"
    "     * `if (rng == 0) rng = 1 + tag;`. So config.seed 0 reproduces the\n"
    "     * CPU default per-env seeds [1, 1, 2, 3, ...]; any non-zero seed\n"
    "     * takes the seed + index form. */\n"
    "    bt_u64 raw = (bt_u64)cfg.seed + (bt_u64)idx;\n"
    "    bt_u64 effective = (raw == (bt_u64)0) ? ((bt_u64)1 + (bt_u64)idx) : raw;\n"
    "    bt_core_init(s, effective, cfg.zone, cfg.shop_steps,\n"
    "                 (unsigned short)cfg.character, (unsigned short)cfg.starting_weapon,\n"
    "                 (unsigned char)(cfg.audio_rng ? 1 : 0));\n"
    "    s->particles_rng = (unsigned char)(cfg.particles_rng ? 1 : 0);\n"
    "    s->invulnerable = (unsigned char)(cfg.invulnerable ? 1 : 0);\n"
    "    s->endless = (unsigned char)(cfg.endless ? 1 : 0);\n"
    "    bt_core_observe(s, obs + idx * BT_OBS_SIZE);\n"
    "    rewards[idx] = 0.0f;\n"
    "    terminals[idx] = 0.0f;\n"
    "}\n"
    "\n"
    "kernel void brotato_metal_reset_kernel(\n"
    "    device BtState* states [[buffer(0)]],\n"
    "    device float* obs [[buffer(1)]],\n"
    "    device float* rewards [[buffer(2)]],\n"
    "    device float* terminals [[buffer(3)]],\n"
    "    constant BrotatoMetalDispatch& args [[buffer(4)]],\n"
    "    uint gid [[thread_position_in_grid]])\n"
    "{\n"
    "    if (gid >= uint(args.count)) return;\n"
    "    int idx = args.start + int(gid);\n"
    "    device BtState* s = &states[idx];\n"
    "    bt_core_reset(s);\n"
    "    bt_core_observe(s, obs + idx * BT_OBS_SIZE);\n"
    "    rewards[idx] = 0.0f;\n"
    "    terminals[idx] = 0.0f;\n"
    "}\n"
    "\n"
    "kernel void brotato_metal_step_kernel(\n"
    "    device BtState* states [[buffer(0)]],\n"
    "    device float* obs [[buffer(1)]],\n"
    "    device const float* actions [[buffer(2)]],\n"
    "    device float* rewards [[buffer(3)]],\n"
    "    device float* terminals [[buffer(4)]],\n"
    "    constant BrotatoMetalDispatch& args [[buffer(5)]],\n"
    "    uint gid [[thread_position_in_grid]])\n"
    "{\n"
    "    if (gid >= uint(args.count)) return;\n"
    "    int idx = args.start + int(gid);\n"
    "    device BtState* s = &states[idx];\n"
    /* The CPU env passes its persistent Agent.rewards[0] / terminals[0], so the
     * caller's slot is part of the observable state: bt_core_step assigns
     * *reward on every path now, but seeding the thread-local from the buffer
     * keeps this backend equal to the CPU env even if a path ever leaves the
     * slot untouched (before the core fix, the level-up non-buy tick leaked the
     * previous tick's value and zero-initialising here diverged). */
    "    thread float reward = rewards[idx];\n"
    "    thread float terminal = terminals[idx];\n"
    "    bt_core_step(s, (int)actions[idx], obs + idx * BT_OBS_SIZE, &reward, &terminal);\n"
    "    rewards[idx] = reward;\n"
    "    terminals[idx] = terminal;\n"
    "}\n"
    "\n"
    "/* Reports the MSL view of BtState so the host can assert its own view\n"
    " * matches. Metal's runtime compiler caches by SOURCE TEXT, and our source\n"
    " * is the includes above (by absolute path) plus this kernel text, so a\n"
    " * header edit can silently reuse a library built against an older BtState\n"
    " * layout. That mis-strides the per-env state buffer (obs/rewards/terminals\n"
    " * live in their own buffers and stay correct, so only strided BtState reads\n"
    " * - the numeric log - come out as garbage) and lets neighbouring envs\n"
    " * overlap. The include-closure fingerprint above prevents the stale hit;\n"
    " * this asserts it. */\n"
    "kernel void brotato_metal_layout_kernel(device unsigned long* out [[buffer(0)]],\n"
    "                                        device uchar* probe [[buffer(1)]])\n"
    "{\n"
    "    device BtState* s = (device BtState*)probe;\n"
    "    out[0] = (unsigned long)sizeof(BtState);\n"
    "    out[1] = (unsigned long)((device uchar*)&s->log - probe);\n"
    "    out[2] = (unsigned long)((device uchar*)&s->phys - probe);\n"
    "    out[3] = (unsigned long)sizeof(BtPhysWorld);\n"
    "    /* Owned-weapon block: pins the profile/loadout offsets the host snapshot\n"
    "     * reads, so a stale Metal library cannot mis-stride the per-slot arrays. */\n"
    "    out[4] = (unsigned long)((device uchar*)&s->wpn_id - probe);\n"
    "    out[5] = (unsigned long)((device uchar*)&s->wpn_cd - probe);\n"
    "}\n";

static std::string bt_metal_library_source() {
    std::string source;
    source += "#include <metal_stdlib>\n";
    source += "using namespace metal;\n";
    /* Marker for any core header that wants to branch on the Metal backend
     * independently of __METAL_VERSION__. */
    source += "#define BT_METAL 1\n";
    /* Cache key: the local include closure fingerprint (see above). */
    source += "/* bt-core-closure "; source += bt_metal_closure_hash_hex(); source += " */\n";
    /* The core is one numerical definition across CPU/CUDA/Metal, so the FP
     * contraction of each backend has to match. The host builds with
     * -ffp-contract=off (build.sh) and CUDA with -fmad=false, so the MSL side
     * pins the same: without this the MSL compiler fuses multiply-adds and the
     * trajectories drift by 1 ulp, which a chaotic sim amplifies into different
     * discrete decisions (different spawn/kill outcomes) after ~1000 steps. */
    source += "#pragma clang fp contract(off)\n";
    /* NOTE ON PRECISION: Metal's math library is not bit-identical to libm
     * (measured over 4096 inputs: metal::sqrt differed from sqrtf on 18% by
     * 1 ulp, sin/cos/atan2 on ~30% by up to 8e-5, even with MTLMathModeSafe;
     * metal::precise::sqrt is exact). The backend cannot fix that from here:
     * brotato_core.h spells its helpers unqualified (bt_sqrtf(x) -> sqrt(x))
     * while brotato_physics_math.h spells them qualified (metal::sqrt(x)), so a
     * plain-name macro like `#define sqrt metal::precise::sqrt` turns the
     * latter into `metal::metal::precise::sqrt` and fails to compile. The
     * precise:: mapping therefore has to live in those two headers, which is
     * what was requested from SharedCore. */
    /* The generated tables carry source literals verbatim (e.g. the 999
     * "unbounded" spawn count), which is a narrowing conversion in C++ and
     * therefore an error in MSL. The host build silences it with
     * -Wno-narrowing (build.sh); the MSL frontend has no flag channel, so the
     * diagnostic is suppressed around the includes only. The conversion is
     * identical on every backend (truncation), so parity is unaffected. */
    source += "#pragma clang diagnostic push\n";
    source += "#pragma clang diagnostic ignored \"-Wc++11-narrowing\"\n";
    source += "#pragma clang diagnostic ignored \"-Wbraced-scalar-init\"\n";
    source += "#pragma clang diagnostic ignored \"-Wconstant-conversion\"\n";
    /* COMPILE-GRAPH MINIMIZATION. Crash reports for this backend show
     * MTLCompilerService aborting with std::bad_alloc inside
     * llvm::CloneBasicBlock, called from a per-function pass of
     * AGXCompilerCore's compile plan: the backend clones basic blocks of the
     * one function the frontend produces when the whole core is inlined into
     * the kernel (and the core's own helper bodies are inlined into each
     * other), and the allocation grows until the compiler service dies. The
     * practical consequence is the multi-minute stall in
     * newComputePipelineStateWithFunction.
     *
     * The core takes BT_HD from the backend (brotato_rng.h only defines it
     * when it is not already defined), so pin it to `noinline` for the core
     * include: every core function stays its own function instead of being
     * merged into one megafunction. That bounds the number of basic blocks any
     * single compiled function has, which is what a block-cloning pass scales
     * with, and removes the duplicated expansion of the phase entry points and
     * bt_core_observe that the old single kernel paid for every mode. No
     * semantics change - `noinline` is a pure optimization barrier, FP
     * contraction is pinned off either way.
     *
     * brotato_physics_math.h is included first with the default (empty) BT_HD
     * so its scalar/Vector2 primitives (bt_minf, bt_maxf, bt_v2 helpers, bt_clampf,
     * only functions that sit inside the hottest per-body loops) keep their
     * `static inline` form and still inline into their callers. Its include
     * guard makes the later include from brotato_physics.h a no-op. */
    const std::string core_dir = bt_metal_dirname(brotato_metal_core_header());
    source += "#include \"" + bt_metal_quote_include_path(core_dir + "/brotato_physics_math.h") + "\"\n";
    source += "#undef BT_HD\n";
    source += "#define BT_HD __attribute__((noinline))\n";
    /* One include: brotato_core.h pulls brotato_state.h -> brotato_rng.h (the
     * BT_TABLE/BT_DEVICE/BT_THREAD/bt_u64 prelude) and then the canonical
     * device-portable tables, then the physics/geometry the state embeds. No
     * backend-specific table mirror exists any more. */
    source += "#include \"" + bt_metal_quote_include_path(brotato_metal_core_header()) + "\"\n";
    source += "#pragma clang diagnostic pop\n";
    source += BROTATO_METAL_KERNEL_SOURCE;
    return source;
}

/* ------------------------------------------------------------------------- */
/* Handle                                                                    */
/* ------------------------------------------------------------------------- */

struct BrotatoMetal {
    int count;
    BrotatoMetalConfig config;
    id<MTLCommandQueue> queue;
    id<MTLComputePipelineState> pipeline_init, pipeline_reset, pipeline_step;
    id<MTLBuffer> states, observations, actions, rewards, terminals;
};

struct BrotatoMetalDispatch { int start, count; };

/* The MSL structs are declared as raw text, so pin the host layout they mirror. */
static_assert(sizeof(BrotatoMetalConfig) == 32, "BrotatoMetalConfig must stay 32 bytes to match the MSL mirror");
static_assert(offsetof(BrotatoMetalConfig, seed) == 0, "BrotatoMetalConfig.seed must lead the struct");
static_assert(sizeof(BrotatoMetalDispatch) == 8, "BrotatoMetalDispatch must stay 8 bytes to match the MSL mirror");

enum { BT_METAL_INIT = 0, BT_METAL_RESET = 1, BT_METAL_STEP = 2 };

static void bt_metal_require(bool ok, const char *message) {
    if (!ok) throw std::invalid_argument(message);
}

static void bt_metal_check_buffer(id<MTLDevice> device, id<MTLBuffer> buffer,
                                  size_t bytes, const char *what) {
    if (!buffer) {
        std::string m = "Brotato Metal: missing buffer: ";
        throw std::invalid_argument(m + what);
    }
    if (buffer.device != device) {
        std::string m = "Brotato Metal: buffer on another device: ";
        throw std::invalid_argument(m + what);
    }
    if (buffer.length < bytes) {
        std::string m = "Brotato Metal: buffer too small: ";
        m += what;
        m += " (need ";
        m += std::to_string((unsigned long long)bytes);
        m += " bytes, have ";
        m += std::to_string((unsigned long long)buffer.length);
        m += ")";
        throw std::invalid_argument(m);
    }
}

/* Build one compute pipeline and report which entry point failed. */
static id<MTLComputePipelineState> bt_metal_make_pipeline(id<MTLDevice> device, id<MTLLibrary> library,
                                                          const char *name) {
    id<MTLFunction> function = [library newFunctionWithName:[NSString stringWithUTF8String:name]];
    if (!function) {
        std::string m = "Brotato Metal: kernel not found: ";
        throw std::runtime_error(m + name);
    }
    NSError *error = nil;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
    if (!pipeline) {
        std::string m = "Brotato Metal: pipeline creation failed for ";
        m += name;
        m += ": ";
        m += error.localizedDescription.UTF8String ? error.localizedDescription.UTF8String
                                                   : "unknown error";
        throw std::runtime_error(m);
    }
    return pipeline;
}

/* Encode one phase. `mode` selects the entry point and the buffers that entry
 * point declares; the three kernels have distinct binding layouts because each
 * one only takes what its phase reads and writes. */
static void bt_metal_dispatch(BrotatoMetal *batch, int mode, int start, int count) {
    bt_metal_require(batch != nullptr, "Brotato Metal: null batch");
    bt_metal_require(start >= 0 && count >= 0 && start <= batch->count && count <= batch->count - start,
                     "Brotato Metal: invalid range");
    if (count == 0) return;
    id<MTLComputePipelineState> pipeline =
        mode == BT_METAL_INIT ? batch->pipeline_init
                              : (mode == BT_METAL_RESET ? batch->pipeline_reset : batch->pipeline_step);
    id<MTLCommandBuffer> cmd = [batch->queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [cmd computeCommandEncoder];
    if (!cmd || !encoder) throw std::runtime_error("Brotato Metal: command allocation failed");
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:batch->states offset:0 atIndex:0];
    [encoder setBuffer:batch->observations offset:0 atIndex:1];
    BrotatoMetalDispatch args{start, count};
    if (mode == BT_METAL_STEP) {
        [encoder setBuffer:batch->actions offset:0 atIndex:2];
        [encoder setBuffer:batch->rewards offset:0 atIndex:3];
        [encoder setBuffer:batch->terminals offset:0 atIndex:4];
        [encoder setBytes:&args length:sizeof(args) atIndex:5];
    } else if (mode == BT_METAL_INIT) {
        [encoder setBuffer:batch->rewards offset:0 atIndex:2];
        [encoder setBuffer:batch->terminals offset:0 atIndex:3];
        [encoder setBytes:&batch->config length:sizeof(batch->config) atIndex:4];
        [encoder setBytes:&args length:sizeof(args) atIndex:5];
    } else {
        [encoder setBuffer:batch->rewards offset:0 atIndex:2];
        [encoder setBuffer:batch->terminals offset:0 atIndex:3];
        [encoder setBytes:&args length:sizeof(args) atIndex:4];
    }
    NSUInteger threads = pipeline.maxTotalThreadsPerThreadgroup;
    if (threads > 256) threads = 256;
    [encoder dispatchThreads:MTLSizeMake((NSUInteger)count, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads, 1, 1)];
    [encoder endEncoding];
    [cmd commit];
    [cmd waitUntilCompleted];
    if (cmd.status != MTLCommandBufferStatusCompleted) {
        std::string m = "Brotato Metal: dispatch failed: ";
        m += cmd.error.localizedDescription.UTF8String ?: "unknown error";
        throw std::runtime_error(m);
    }
}

BrotatoMetal *brotato_metal_create(id<MTLDevice> device, int count,
                                   BrotatoMetalConfig config,
                                   id<MTLBuffer> observations, id<MTLBuffer> actions,
                                   id<MTLBuffer> rewards, id<MTLBuffer> terminals) {
    bt_metal_require(device != nil, "Brotato Metal: null device");
    bt_metal_require(count > 0, "Brotato Metal: count must be positive");

    bt_metal_check_buffer(device, observations, (size_t)count * BT_OBS_SIZE * sizeof(float), "observations");
    bt_metal_check_buffer(device, actions, (size_t)count * sizeof(float), "actions");
    bt_metal_check_buffer(device, rewards, (size_t)count * sizeof(float), "rewards");
    bt_metal_check_buffer(device, terminals, (size_t)count * sizeof(float), "terminals");

    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    /* Safe math keeps the MSL helpers as close to the CPU float results as the
     * compiler allows; the parity test measures the residual. */
    options.mathMode = MTLMathModeSafe;
    /* The effect graph is large; keep generated shader code compact without
     * relaxing the arithmetic contract above. */
    options.optimizationLevel = MTLLibraryOptimizationLevelSize;
    std::string library_source = bt_metal_library_source();
    NSString *source = [NSString stringWithUTF8String:library_source.c_str()];
    if (!source) throw std::runtime_error("Brotato Metal: MSL source is not valid UTF-8");
    id<MTLLibrary> library = [device newLibraryWithSource:source options:options error:&error];
    if (!library)
        throw std::runtime_error(error.localizedDescription.UTF8String
                                     ? error.localizedDescription.UTF8String
                                     : "Brotato Metal: MSL compilation failed");
    /* One pipeline per phase (see BROTATO_METAL_KERNEL_SOURCE): each entry
     * point is compiled on its own, so no single backend function ever
     * contains more than one phase. */
    id<MTLComputePipelineState> pipeline_init =
        bt_metal_make_pipeline(device, library, "brotato_metal_init_kernel");
    id<MTLComputePipelineState> pipeline_reset =
        bt_metal_make_pipeline(device, library, "brotato_metal_reset_kernel");
    id<MTLComputePipelineState> pipeline_step =
        bt_metal_make_pipeline(device, library, "brotato_metal_step_kernel");

    /* The kernel must agree with this translation unit on BtState's size and
     * member offsets, or every env beyond the first is read at the wrong
     * address (see brotato_metal_layout_kernel). */
    {
        id<MTLComputePipelineState> lpipeline =
            bt_metal_make_pipeline(device, library, "brotato_metal_layout_kernel");
        id<MTLBuffer> lout = [device newBufferWithLength:8 * sizeof(unsigned long)
                                                options:MTLResourceStorageModeShared];
        id<MTLBuffer> lprobe = [device newBufferWithLength:sizeof(BtState) options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> lqueue = [device newCommandQueue];
        if (!lout || !lprobe || !lqueue) throw std::runtime_error("Brotato Metal: layout probe allocation failed");
        id<MTLCommandBuffer> lcb = [lqueue commandBuffer];
        id<MTLComputeCommandEncoder> le = [lcb computeCommandEncoder];
        [le setComputePipelineState:lpipeline];
        [le setBuffer:lout offset:0 atIndex:0];
        [le setBuffer:lprobe offset:0 atIndex:1];
        [le dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
        [le endEncoding];
        [lcb commit];
        [lcb waitUntilCompleted];
        if (lcb.status != MTLCommandBufferStatusCompleted)
            throw std::runtime_error("Brotato Metal: layout probe dispatch failed");
        const unsigned long *got = (const unsigned long *)lout.contents;
        const unsigned long want[6] = {(unsigned long)sizeof(BtState), (unsigned long)offsetof(BtState, log),
                                       (unsigned long)offsetof(BtState, phys), (unsigned long)sizeof(BtPhysWorld),
                                       (unsigned long)offsetof(BtState, wpn_id), (unsigned long)offsetof(BtState, wpn_cd)};
        const char *names[6] = {"sizeof(BtState)", "offsetof(log)", "offsetof(phys)", "sizeof(BtPhysWorld)",
                                "offsetof(wpn_id)", "offsetof(wpn_cd)"};
        for (int i = 0; i < 6; ++i) {
            if (got[i] == want[i]) continue;
            char msg[256];
            std::snprintf(msg, sizeof(msg),
                          "Brotato Metal: kernel/host BtState layout mismatch: %s is %lu on the device and %lu on "
                          "the host (stale Metal library cache?)",
                          names[i], got[i], want[i]);
            throw std::runtime_error(msg);
        }
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLBuffer> states = [device newBufferWithLength:(size_t)count * sizeof(BtState)
                                               options:MTLResourceStorageModeShared];
    if (!queue || !states) throw std::runtime_error("Brotato Metal: allocation failed");

    BrotatoMetal *batch = new BrotatoMetal{count, config, queue, pipeline_init, pipeline_reset, pipeline_step,
                                           states, observations, actions, rewards, terminals};

    /* Deterministic bring-up: zero storage, then let the kernel run bt_core_init
     * for every env. Zeroing first means a backend that does not write a field
     * during init still matches a zero-initialised CPU reference exactly. */
    std::memset(states.contents, 0, (size_t)count * sizeof(BtState));
    try {
        bt_metal_dispatch(batch, BT_METAL_INIT, 0, count);
    } catch (...) {
        delete batch;
        throw;
    }
    return batch;
}

void brotato_metal_reset(BrotatoMetal *batch) {
    bt_metal_require(batch != nullptr, "Brotato Metal: null batch");
    bt_metal_dispatch(batch, BT_METAL_RESET, 0, batch->count);
}

void brotato_metal_step(BrotatoMetal *batch, int start, int count) {
    bt_metal_dispatch(batch, BT_METAL_STEP, start, count);
}

BrotatoMetalState brotato_metal_state(BrotatoMetal *batch, int index) {
    bt_metal_require(batch != nullptr, "Brotato Metal: null batch");
    bt_metal_require(index >= 0 && index < batch->count, "Brotato Metal: state index out of range");
    const BtState *s = (const BtState *)batch->states.contents + index;
    BrotatoMetalState out{};
    out.log.perf = s->log.perf;
    out.log.score = s->log.score;
    out.log.episode_return = s->log.episode_return;
    out.log.episode_length = s->log.episode_length;
    out.log.n = s->log.n;
    out.hp = s->hp;
    out.max_hp = s->max_hp;
    out.px = s->px;
    out.py = s->py;
    out.iframe = s->iframe;
    out.time_left = s->time_left;
    out.wave_duration = s->wave_duration;
    out.episode_return = s->episode_return;
    out.dmg_taken = s->dmg_taken;
    out.materials = s->materials;
    out.xp = s->xp;
    out.arm_max_hp = s->armor;
    out.phase = s->phase;
    out.wave = s->wave;
    out.kills = s->kills;
    out.level = s->level;
    out.xp_need = s->xp_need;
    out.ecount = s->ecount;
    out.ticks = s->ticks;
    out.tick_in_wave = s->tick_in_wave;
    out.waves_cleared = s->waves_cleared;
    out.done = s->done;
    out.won = s->won;
    for (int i = 0; i < 4; ++i) {
        out.shop_kind[i] = s->shop_kind[i];
        out.shop_id[i] = s->shop_id[i];
        out.shop_cursed[i] = s->shop_cursed[i];
        out.lvl_offers[i] = s->lvl_offers[i];
    }
    out.character = s->character;
    out.weapon_slot_limit = s->weapon_slot_limit;
    out.starting_weapon = s->starting_weapon;
    out.wpn_count = s->wpn_count;
    out.wpn_overflow = s->wpn_overflow;
    out.wpn_uid_next = s->wpn_uid_next;
    for (int i = 0; i < BT_SLOTS; ++i) {
        out.wpn_id[i] = s->wpn_id[i];
        out.wpn_tier[i] = s->wpn_tier[i];
        out.wpn_cursed[i] = s->wpn_cursed[i];
        out.wpn_curse[i] = s->wpn_curse[i];
        out.wpn_tracked[i] = s->wpn_tracked[i];
        out.wpn_uid[i] = s->wpn_uid[i];
        out.wpn_cd[i] = s->wpn_cd[i];
        out.wpn_melee_anim[i] = s->wpn_melee_anim[i];
        out.wpn_melee_phase[i] = s->wpn_melee_phase[i];
        out.wpn_melee_hit_on[i] = s->wpn_melee_hit_on[i];
        out.wpn_melee_shooting[i] = s->wpn_melee_shooting[i];
        out.wpn_melee_rot[i] = s->wpn_melee_rot[i];
    }
    out.rng_state = (unsigned long long)s->rng.state;
    out.rng_ready = s->rng_ready;
    return out;
}

BrotatoMetalLog brotato_metal_log(BrotatoMetal *batch, bool clear) {
    bt_metal_require(batch != nullptr, "Brotato Metal: null batch");
    BtState *states = (BtState *)batch->states.contents;
    BrotatoMetalLog sum{};
    for (int i = 0; i < batch->count; ++i) {
        BtState *s = states + i;
        sum.perf += s->log.perf;
        sum.score += s->log.score;
        sum.episode_return += s->log.episode_return;
        sum.episode_length += s->log.episode_length;
        sum.n += s->log.n;
        if (clear) {
            s->log.perf = 0.0f;
            s->log.score = 0.0f;
            s->log.episode_return = 0.0f;
            s->log.episode_length = 0.0f;
            s->log.n = 0.0f;
        }
    }
    return sum;
}

void brotato_metal_close(BrotatoMetal *batch) { delete batch; }

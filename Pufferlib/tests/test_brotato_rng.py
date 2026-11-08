"""Compare portable RNG outputs with the pinned Godot engine, not copied vectors.

Opt in with BROTATO_TEST_ENGINE=/path/to/Godot (Godot 3.7-dev1, the pinned
commit 0a9011743).

Two streams are covered, because the game uses two:

  * RandomNumberGenerator (Utils.gd keeps its own instance, Utils._rng):
    randi / randi_range / randf / randf_range.
  * the global Math::default_rand stream behind the GDScript built-ins:
    randi() / randf() / rand_range().

Floats are compared as raw bit patterns, not decimals: the GDScript side
serialises with StreamPeerBuffer (little-endian IEEE bytes) and
PoolByteArray.hex_encode(), and the native side emits the same little-endian
byte order, so any mismatch is a real value difference rather than a
decimal-formatting artefact. Zero outputs and equal/reversed/negative bounds
are exercised so the zero-draw short circuits and the reversal branch are
covered.
"""
import json
import os
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[1]
ENGINE = os.environ.get("BROTATO_TEST_ENGINE")
requires_engine = pytest.mark.skipif(not ENGINE, reason="set BROTATO_TEST_ENGINE to Godot 3.7-dev1")


def _godot_script(tmp_path, name, body, seed, output):
    """Run an inline GDScript with --seed/--out and return its JSON object."""
    script = tmp_path / name
    script.write_text(body)
    result = subprocess.run(
        [ENGINE, "--path", str(ROOT / "tests/brotato_physics_project"), "--no-window",
         "--audio-driver", "Dummy", "--script", str(script),
         "--seed=" + str(seed), "--out=" + str(output)],
        text=True, capture_output=True, timeout=30,
    )
    assert result.returncode == 0, result.stdout + result.stderr
    return json.loads(Path(output).read_text())


@pytest.fixture(scope="module")
def native_rng(tmp_path_factory):
    directory = tmp_path_factory.mktemp("brotato-native-rng")
    source = directory / "rng.c"
    source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include "ocean/brotato/brotato_rng.h"
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    BtRng rng;
    bt_rng_seed(&rng, (bt_u64)strtoull(argv[1], 0, 10));
    printf("[");
    for (int i = 0; i < 16; i++) {
        unsigned int raw = bt_rng_next(&rng);
        int ranged = (int)bt_rng_bounded(&rng, 61u) - 30;
        int singleton = (int)bt_rng_bounded(&rng, 1u) + 7;
        unsigned int after_singleton = bt_rng_next(&rng);
        printf("%s[%u,%d,%d,%u]", i ? "," : "", raw, ranged, singleton, after_singleton);
    }
    puts("]");
    return 0;
}
''')
    binary = directory / "rng"
    subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O2", "-I", str(ROOT),
                    str(source), "-o", str(binary)], check=True, capture_output=True, text=True)
    return binary


@requires_engine
@pytest.mark.parametrize("seed", [0, 1, 42, 2**63 - 1, -1])
def test_integer_stream_matches_godot(native_rng, tmp_path, seed):
    script = '''extends SceneTree
func _init():
    var value = 0
    var path = ""
    for arg in OS.get_cmdline_args():
        if arg.begins_with("--seed="):
            value = int(arg.substr(7))
        elif arg.begins_with("--out="):
            path = arg.substr(6)
    var rng = RandomNumberGenerator.new()
    rng.seed = value
    var rows = []
    for _i in range(16):
        var raw = rng.randi()
        var ranged = rng.randi_range(-30, 30)
        var singleton = rng.randi_range(7, 7)
        var after_singleton = rng.randi()
        rows.append([raw, ranged, singleton, after_singleton])
    var output = File.new()
    if output.open(path, File.WRITE) != OK:
        quit(2)
        return
    output.store_string(JSON.print(rows))
    output.close()
    quit(0)
'''
    godot = _godot_script(tmp_path, "rng.gd", script, seed, tmp_path / "godot.json")
    native = subprocess.run([str(native_rng), str(seed)], check=True,
                            capture_output=True, text=True)
    assert json.loads(native.stdout) == godot


@pytest.fixture(scope="module")
def native_float_rng(tmp_path_factory):
    directory = tmp_path_factory.mktemp("brotato-native-float-rng")
    source = directory / "float_rng.c"
    source.write_text(r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ocean/brotato/brotato_rng.h"

/* Little-endian byte hex, the same layout Godot's StreamPeerBuffer serialises
 * floats in, so the two sides can be compared bit for bit. */
static void hex_le(bt_u64 u, int bytes, char *out) {
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < bytes; i++) {
        unsigned int b = (unsigned int)((u >> (8 * i)) & 0xffu);
        out[2 * i] = H[b >> 4];
        out[2 * i + 1] = H[b & 15u];
    }
    out[2 * bytes] = 0;
}
static void hex_f32(float v, char *out) { unsigned int u; memcpy(&u, &v, 4); hex_le((bt_u64)u, 4, out); }
static void hex_f64(bt_f64 v, char *out) { bt_u64 u; memcpy(&u, &v, 8); hex_le(u, 8, out); }

/* Equal, reversed and negative bounds; (0,0) and (5,5) still consume draws. */
static const float BOUNDS[6][2] = {
    {0.0f, 1.0f}, {-1.0f, 1.0f}, {5.0f, 5.0f}, {360.0f, 0.0f}, {-7.0f, -3.0f}, {0.0f, 0.0f}
};

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    bt_u64 seed = (bt_u64)strtoull(argv[1], 0, 10);
    char b[17];

    printf("{");

    /* RandomNumberGenerator stream: 8x randf, 6x randf_range, 3x randi_range, 4x randf. */
    BtRng r;
    bt_rng_seed(&r, seed);
    printf("\"rng_randf\":[");
    for (int i = 0; i < 8; i++) { hex_f32(bt_rng_randf(&r), b); printf("%s\"%s\"", i ? "," : "", b); }
    printf("],\"rng_range\":[");
    for (int i = 0; i < 6; i++) { hex_f32(bt_rng_random_f(&r, BOUNDS[i][0], BOUNDS[i][1]), b); printf("%s\"%s\"", i ? "," : "", b); }
    int ri0 = bt_rng_randi_range(&r, -30, 30);
    int ri1 = bt_rng_randi_range(&r, 10, 2);
    int ri2 = bt_rng_randi_range(&r, 7, 7);
    printf("],\"rng_int\":[%d,%d,%d],\"rng_tail\":[", ri0, ri1, ri2);
    for (int i = 0; i < 4; i++) { hex_f32(bt_rng_randf(&r), b); printf("%s\"%s\"", i ? "," : "", b); }
    printf("],");

    /* Global Math::default_rand stream: 8x randf, 6x rand_range (FP64), 2x randi.
     * The pinned engine is a single-precision build (real_t == float), so
     * RandomNumberGenerator.randf_range uses FP32 while the global rand_range
     * keeps Math::random(double, double)'s FP64 arithmetic. */
    BtRng g;
    bt_rng_seed(&g, seed);
    printf("\"global_randf\":[");
    for (int i = 0; i < 8; i++) { hex_f32(bt_math_randf(&g), b); printf("%s\"%s\"", i ? "," : "", b); }
    printf("],\"global_range\":[");
    for (int i = 0; i < 6; i++) {
        hex_f64(bt_rng_random_d(&g, (bt_f64)BOUNDS[i][0], (bt_f64)BOUNDS[i][1]), b);
        printf("%s\"%s\"", i ? "," : "", b);
    }
    unsigned int gi0 = bt_rng_next(&g);
    unsigned int gi1 = bt_rng_next(&g);
    printf("],\"global_int\":[%u,%u]", gi0, gi1);

    printf("}\n");
    return 0;
}
''')
    binary = directory / "float_rng"
    subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O2", "-I", str(ROOT),
                    str(source), "-o", str(binary)], check=True, capture_output=True, text=True)
    return binary


GODOT_FLOAT_SCRIPT = '''extends SceneTree

func _hex32(x) -> String:
    var sp := StreamPeerBuffer.new()
    sp.put_float(x)
    return sp.data_array.hex_encode()

func _hex64(x) -> String:
    var sp := StreamPeerBuffer.new()
    sp.put_double(x)
    return sp.data_array.hex_encode()

func _init() -> void:
    var value := 0
    var path := ""
    for arg in OS.get_cmdline_args():
        if arg.begins_with("--seed="):
            value = int(arg.substr(7))
        elif arg.begins_with("--out="):
            path = arg.substr(6)
    var bounds := [[0.0, 1.0], [-1.0, 1.0], [5.0, 5.0], [360.0, 0.0], [-7.0, -3.0], [0.0, 0.0]]
    var out := {}

    var rng := RandomNumberGenerator.new()
    rng.seed = value
    var r_randf := []
    for _i in range(8):
        r_randf.append(_hex32(rng.randf()))
    var r_range := []
    for b in bounds:
        r_range.append(_hex32(rng.randf_range(b[0], b[1])))
    var r_int := [rng.randi_range(-30, 30), rng.randi_range(10, 2), rng.randi_range(7, 7)]
    var r_tail := []
    for _i in range(4):
        r_tail.append(_hex32(rng.randf()))
    out["rng_randf"] = r_randf
    out["rng_range"] = r_range
    out["rng_int"] = r_int
    out["rng_tail"] = r_tail

    seed(value)
    var g_randf := []
    for _i in range(8):
        g_randf.append(_hex32(randf()))
    var g_range := []
    for b in bounds:
        g_range.append(_hex64(rand_range(b[0], b[1])))
    out["global_randf"] = g_randf
    out["global_range"] = g_range
    out["global_int"] = [randi(), randi()]

    var output := File.new()
    if output.open(path, File.WRITE) != OK:
        quit(2)
        return
    output.store_string(JSON.print(out))
    output.close()
    quit(0)
'''


@requires_engine
@pytest.mark.parametrize("seed", [0, 1, 42, 2**63 - 1, -1])
def test_float_draws_match_godot(native_float_rng, tmp_path, seed):
    godot = _godot_script(tmp_path, "float_rng.gd", GODOT_FLOAT_SCRIPT, seed, tmp_path / "godot.json")
    native = subprocess.run([str(native_float_rng), str(seed)], check=True,
                            capture_output=True, text=True)
    assert json.loads(native.stdout) == godot


def test_helper_edge_semantics(tmp_path):
    """Native-only pinning of the properties the engine comparison cannot force.

    The engine draws never hit RandomPCG's proto_exp_offset == 0 short circuit
    (probability 2^-32), so the endpoint/draw-count contract is asserted
    directly: equal bounds return the bound exactly, reversed bounds stay inside
    the reversed interval, and each helper consumes the engine's exact draw
    count (randf/randf_range 2, randi_range 1). CLZ32 edge cases are pinned too.
    """
    source = tmp_path / "edges.c"
    source.write_text(r'''
#include <stdio.h>
#include "ocean/brotato/brotato_rng.h"

int main(void) {
    if (bt_rng_clz32(0u) != 32u) return 1;
    if (bt_rng_clz32(1u) != 31u) return 2;
    if (bt_rng_clz32(0x80000000u) != 0u) return 3;
    if (bt_rng_clz32(0x0000FFFFu) != 16u) return 4;

    BtRng r;
    bt_rng_seed(&r, 7);

    /* Equal bounds are returned exactly (and still consume two draws). */
    const float eq[4] = {0.0f, -7.5f, 5.0f, 1.0e9f};
    for (int i = 0; i < 4; i++) {
        if (bt_rng_random_f(&r, eq[i], eq[i]) != eq[i]) return 10 + i;
    }

    /* randf_range / randf consume exactly two pcg draws. */
    BtRng a = r;
    float rev = bt_rng_random_f(&r, 3.0f, -4.0f);
    if (!(rev >= -4.0f && rev <= 3.0f)) return 20;
    BtRng b = r;
    BtRng c = a;
    (void)bt_rng_next(&c);
    (void)bt_rng_next(&c);
    if (b.state != c.state || b.inc != c.inc) return 21;

    /* randi_range consumes exactly one draw, including reversed bounds. */
    BtRng d = r;
    int ri = bt_rng_randi_range(&r, 9, 4);
    if (ri < 4 || ri > 9) return 22;
    BtRng e = r;
    BtRng f = d;
    (void)bt_rng_next(&f);
    if (e.state != f.state || e.inc != f.inc) return 23;

    /* The global Math::randf path consumes exactly one draw. */
    BtRng g = r;
    float mf = bt_math_randf(&r);
    if (!(mf >= 0.0f && mf <= 1.0f)) return 24;
    BtRng h = r;
    BtRng k = g;
    (void)bt_rng_next(&k);
    if (h.state != k.state || h.inc != k.inc) return 25;

    puts("ok");
    return 0;
}
''')
    binary = tmp_path / "edges"
    subprocess.run([os.environ.get("CC", "clang"), "-std=c11", "-O2", "-I", str(ROOT),
                    str(source), "-o", str(binary)], check=True, capture_output=True, text=True)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, f"edge helper assertion {result.returncode}"


def test_software_binary64_matches_hardware_ranges(tmp_path):
    """Metal's integer implementation must preserve narrowed bits and decisions."""
    source = tmp_path / "software_ranges.c"
    source.write_text(r'''
#define BT_RNG_FORCE_SOFT_F64
#include "ocean/brotato/brotato_rng.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    BtRng bounds, software, hardware;
    bt_rng_seed(&bounds, 1927);
    for (unsigned int i = 0; i < 100000; i++) {
        union { unsigned int u; float f; } a, b;
        a.u = bt_rng_next(&bounds); b.u = bt_rng_next(&bounds);
        if (((a.u >> 23) & 255) == 255 || ((b.u >> 23) & 255) == 255) continue;
        if (i % 17 == 0) b.f = a.f;
        bt_rng_seed(&software, i); bt_rng_seed(&hardware, i);
        float actual = bt_rng_global_range_float(&software, a.f, b.f);
        double wide = bt_rng_random_d(&hardware, (double)a.f, (double)b.f);
        float expected = (float)wide;
        if (memcmp(&actual, &expected, sizeof(float)) || software.state != hardware.state) {
            printf("float mismatch at %u: %a,%a -> %a versus %a\n", i, a.f, b.f, actual, expected);
            return 1;
        }
        if (wide > -2147483648.0 && wide < 2147483647.0) {
            bt_rng_seed(&software, i); bt_rng_seed(&hardware, i);
            int x = bt_rng_global_range_int(&software, a.f, b.f);
            int y = (int)bt_rng_random_d(&hardware, a.f, b.f);
            if (x != y || software.state != hardware.state) return 2;
        }
    }
    const float edges[][2] = {{0.0f,-0.0f},{-0.0f,-0.0f},{5,5},{360,0},
                              {-50,50},{-1e-40f,1e-40f}};
    for (unsigned int i = 0; i < sizeof(edges)/sizeof(edges[0]); i++) {
        bt_rng_seed(&software, 0); bt_rng_seed(&hardware, 0);
        float x = bt_rng_global_range_float(&software, edges[i][0], edges[i][1]);
        float y = (float)bt_rng_random_d(&hardware, edges[i][0], edges[i][1]);
        if (memcmp(&x, &y, sizeof(float))) return 3;
    }
    return 0;
}
''')
    binary = tmp_path / "software_ranges"
    subprocess.run(
        [os.environ.get("CC", "clang"), "-std=c11", "-O2", "-ffp-contract=off",
         "-I", str(ROOT), str(source), "-o", str(binary)],
        check=True, capture_output=True, text=True,
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr


@requires_engine
@pytest.mark.parametrize("software", [False, True], ids=["hardware", "software"])
def test_script_binary64_bounds_and_member_assignment(tmp_path, software):
    """Keep script literals, post-draw addition and integer assignment exact."""
    source = tmp_path / "script_bounds.c"
    source.write_text(r'''
#include "ocean/brotato/brotato_rng.h"
#include <stdio.h>
static bt_u64 bits(double value) {
    union { double f; bt_u64 u; } r;
    r.f = value; return r.u;
}
static void print_bits(bt_u64 value) {
    for (int i = 0; i < 8; i++) printf("%02x", (unsigned int)((value >> (8 * i)) & 255u));
}
int main(void) {
    const double pi = 3.14159265358979323846, tau = 6.28318530717958647692;
    const double bounds[][2] = {{pi,tau},{-0.2,0.2},{tau,pi},{pi,pi}};
    BtRng rng; bt_rng_seed(&rng, 42);
    printf("[");
    for (int i = 0; i < 4; i++) {
        bt_u64 value = bt_rng_global_range_bits(&rng, bits(bounds[i][0]), bits(bounds[i][1]));
        bt_u64 added = bt_rng_f64_add_bits(value, bits(300.0));
        bt_u64 truncated = bt_rng_f64_trunc_bits(added);
        printf("%s[\"", i ? "," : "");
        print_bits(value); print_bits(added); print_bits(truncated);
        printf("\",%u]", bt_rng_next(&rng));
    }
    puts("]");
    return 0;
}
''')
    binary = tmp_path / "script_bounds"
    command = [os.environ.get("CC", "clang"), "-std=c11", "-O2", "-ffp-contract=off"]
    if software:
        command.append("-DBT_RNG_FORCE_SOFT_F64")
    subprocess.run(command + ["-I", str(ROOT), str(source), "-o", str(binary)],
                   check=True, capture_output=True, text=True)
    script = '''extends SceneTree
func _init():
    var path = ""
    for arg in OS.get_cmdline_args():
        if arg.begins_with("--out="):
            path = arg.substr(6)
    seed(42)
    var rows = []
    for bounds in [[PI, TAU], [-0.2, 0.2], [TAU, PI], [PI, PI]]:
        var value = rand_range(bounds[0], bounds[1])
        var added = value + 300.0
        var truncated = float(int(added))
        var bytes = StreamPeerBuffer.new()
        bytes.put_double(value)
        bytes.put_double(added)
        bytes.put_double(truncated)
        rows.append([bytes.data_array.hex_encode(), randi()])
    var output = File.new()
    if output.open(path, File.WRITE) != OK:
        quit(2)
        return
    output.store_string(JSON.print(rows))
    output.close()
    quit(0)
'''
    godot = _godot_script(tmp_path, "script_bounds.gd", script, 42, tmp_path / "godot.json")
    native = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
    assert json.loads(native.stdout) == godot


def test_engine_source_formulas(tmp_path):
    """Bit-exact differential test against the pinned engine's own expressions.

    The engine-backed tests skip without BROTATO_TEST_ENGINE. This one always
    runs: it re-implements, beside the portable helpers and sharing no code
    with them, the exact expressions of godotengine/godot@0a9011743 (file:line
    cited in the C source) and requires bit-identical values and identical
    PCG-output consumption.

    It also pins the two things an engine comparison cannot reach:
      * RandomPCG::randf/randd's proto_exp_offset == 0 short circuit, forced by
        setting state = 0 (pcg.cpp:6's output function maps old state 0 to a
        0 output), so the zero-draw return and its one-draw cost are checked;
      * the operand order of randd()'s `(((uint64_t)rand()) << 32) | rand()`,
        which the C++ standard leaves unsequenced. godot builds this with
        clang/gcc, which evaluate the shifted operand first on every
        optimisation level (measured); the reference keeps the engine's exact
        expression so a toolchain that chose the other order fails this test
        rather than silently desynchronising bt_rng_randd.
    """
    source = tmp_path / "engine_formulas.c"
    source.write_text(r'''
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ocean/brotato/brotato_rng.h"

/* Reference: verbatim expressions of godotengine/godot@0a9011743.
 *   thirdparty/misc/pcg.h:9            PCG_DEFAULT_INC_64
 *   thirdparty/misc/pcg.cpp:6          pcg32_random_r
 *   thirdparty/misc/pcg.cpp:18         pcg32_srandom_r
 *   core/math/random_pcg.h:69          DEFAULT_INC
 *   core/math/random_pcg.h:73          RandomPCG::seed
 *   core/math/random_pcg.h:83          RandomPCG::rand
 *   core/math/random_pcg.h:100         RandomPCG::randd
 *   core/math/random_pcg.h:113         RandomPCG::randf
 *   core/math/random_pcg.cpp:43        RandomPCG::random(double,double)
 *   core/math/random_pcg.cpp:47        RandomPCG::random(float,float)
 *   core/math/math_funcs.cpp:35        Math::default_rand(DEFAULT_SEED, DEFAULT_INC)
 *   core/math/math_funcs.cpp:46-56     Math::seed / Math::randomize / Math::rand
 *   core/math/math_funcs.h:300-301     Math::randd / Math::randf
 *   core/math/random_number_generator.h:46-71
 *                                      set_seed / randi / randf / randf_range / randi_range / randomize
 *   core/array.cpp:339-351             Array::shuffle
 *   core/array.cpp:420-423             Array::pick_random
 *   modules/gdscript/gdscript_functions.cpp:423-455
 *                                      randomize/randi/randf/rand_range/seed dispatch
 */
typedef struct { uint64_t state, inc; } RefPcg;

static uint32_t ref_next(RefPcg *rng) {
    uint64_t oldstate = rng->state;
    rng->state = oldstate * 6364136223846793005ULL + (rng->inc | 1);
    uint32_t xorshifted = (uint32_t)(((oldstate >> 18u) ^ oldstate) >> 27u);
    uint32_t rot = (uint32_t)(oldstate >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((-rot) & 31));
}

static void ref_srandom(RefPcg *rng, uint64_t initstate, uint64_t initseq) {
    rng->state = 0U;
    rng->inc = (initseq << 1u) | 1u;
    ref_next(rng);
    rng->state += initstate;
    ref_next(rng);
}

static void ref_seed(RefPcg *rng, uint64_t seed) {
    ref_srandom(rng, seed, 1442695040888963407ULL);
}

static float ref_randf(RefPcg *rng) {
    uint32_t proto_exp_offset = ref_next(rng);
    if (proto_exp_offset == 0) return 0;
    return ldexpf((float)(ref_next(rng) | 0x80000001), -32 - (int)__builtin_clz(proto_exp_offset));
}

static double ref_randd(RefPcg *rng) {
    uint32_t proto_exp_offset = ref_next(rng);
    if (proto_exp_offset == 0) return 0;
    uint64_t significand = ((uint64_t)ref_next(rng) << 32) | ref_next(rng) | 0x8000000000000001ULL;
    return ldexp((double)significand, -64 - (int)__builtin_clz(proto_exp_offset));
}

static float ref_math_randf(RefPcg *rng) {
    return (float)ref_next(rng) / (float)0xFFFFFFFF;
}

static double ref_random_d(RefPcg *rng, double from, double to) {
    return ref_randd(rng) * (to - from) + from;
}

static float ref_random_f(RefPcg *rng, float from, float to) {
    return ref_randf(rng) * (to - from) + from;
}

static int ref_randi_range(RefPcg *rng, int from, int to) {
    unsigned int ret = ref_next(rng);
    if (to < from) {
        return (int)(ret % (unsigned int)(from - to + 1)) + to;
    }
    return (int)(ret % (unsigned int)(to - from + 1)) + from;
}

static void ref_shuffle(RefPcg *rng, int *data, int n) {
    if (n < 2) return;                       /* array.cpp:341 early return */
    for (int i = n - 1; i >= 1; i--) {
        const int j = (int)(ref_next(rng) % (uint32_t)(i + 1));
        const int tmp = data[j];
        data[j] = data[i];
        data[i] = tmp;
    }
}

static int ref_pick_random(RefPcg *rng, int size) {
    return (int)(ref_next(rng) % (uint32_t)size);
}

static int fails = 0;
static void check(int cond, const char *what, uint64_t seed, int i) {
    if (!cond) {
        printf("FAIL %s seed=%llu i=%d\n", what, (unsigned long long)seed, i);
        fails++;
    }
}
static int f32_eq(float a, float b) { return memcmp(&a, &b, 4) == 0; }
static int f64_eq(double a, double b) { return memcmp(&a, &b, 8) == 0; }
static int same_state(BtRng *a, RefPcg *b) {
    return a->state == b->state && a->inc == b->inc;
}
static uint64_t bits_of(double v) { uint64_t u; memcpy(&u, &v, 8); return u; }

int main(void) {
    static const uint64_t SEEDS[] = {
        0ull, 1ull, 42ull, 0xFFFFFFFFFFFFFFFFull, 0x8000000000000000ull,
        12047754176567800795ull, 1442695040888963407ull, 0x123456789ABCDEFull,
    };
    const int NSEEDS = (int)(sizeof(SEEDS) / sizeof(SEEDS[0]));

    /* Math::rand / pcg32_random_r stream and seeding (pcg.cpp:6-24). */
    for (int s = 0; s < NSEEDS; s++) {
        BtRng a; RefPcg b;
        bt_rng_seed(&a, SEEDS[s]);
        ref_seed(&b, SEEDS[s]);
        check(a.state == b.state && a.inc == b.inc, "seed state", SEEDS[s], -1);
        for (int i = 0; i < 20000; i++) {
            unsigned int x = bt_rng_next(&a);
            unsigned int y = ref_next(&b);
            if (x != y || !same_state(&a, &b)) { check(0, "pcg stream", SEEDS[s], i); break; }
        }
    }

    /* RandomPCG::randf/randd/random and Math::randf (2 / 2-3 / 1 draws). */
    for (int s = 0; s < NSEEDS; s++) {
        BtRng a; RefPcg b;
        bt_rng_seed(&a, SEEDS[s]); ref_seed(&b, SEEDS[s]);
        for (int i = 0; i < 4000; i++) {
            if (!f32_eq(bt_rng_randf(&a), ref_randf(&b)) || !same_state(&a, &b)) {
                check(0, "RandomPCG::randf", SEEDS[s], i); break;
            }
        }
        for (int i = 0; i < 4000; i++) {
            if (!f64_eq((double)bt_rng_randd(&a), ref_randd(&b)) || !same_state(&a, &b)) {
                check(0, "RandomPCG::randd", SEEDS[s], i); break;
            }
        }
        for (int i = 0; i < 4000; i++) {
            if (!f32_eq(bt_math_randf(&a), ref_math_randf(&b)) || !same_state(&a, &b)) {
                check(0, "Math::randf", SEEDS[s], i); break;
            }
        }
    }

    /* Bounds: equal, reversed, negative, subnormal, zero. */
    static const float FB[][2] = {
        {0.0f, 1.0f}, {-1.0f, 1.0f}, {5.0f, 5.0f}, {360.0f, 0.0f},
        {-7.0f, -3.0f}, {0.0f, 0.0f}, {-0.0f, -0.0f}, {0.0f, -0.0f},
        {-1e-40f, 1e-40f}, {1e9f, -1e9f}, {0.0f, 0.0f},
    };
    const int NFB = (int)(sizeof(FB) / sizeof(FB[0]));
    for (int s = 0; s < NSEEDS; s++) {
        BtRng a; RefPcg b;
        bt_rng_seed(&a, SEEDS[s]); ref_seed(&b, SEEDS[s]);
        for (int i = 0; i < 300; i++) {
            const float lo = FB[i % NFB][0], hi = FB[i % NFB][1];
            if (!f32_eq(bt_rng_random_f(&a, lo, hi), ref_random_f(&b, lo, hi)) || !same_state(&a, &b)) {
                check(0, "RandomPCG::random(float)", SEEDS[s], i); break;
            }
            if (!f64_eq((double)bt_rng_random_d(&a, (double)lo, (double)hi),
                        ref_random_d(&b, (double)lo, (double)hi)) || !same_state(&a, &b)) {
                check(0, "RandomPCG::random(double)", SEEDS[s], i); break;
            }
            if (!f32_eq(bt_rng_global_range_float(&a, lo, hi),
                        (float)ref_random_d(&b, (double)lo, (double)hi)) || !same_state(&a, &b)) {
                check(0, "global rand_range float", SEEDS[s], i); break;
            }
            if (bt_rng_global_range_bits(&a, bits_of((double)lo), bits_of((double)hi)) !=
                bits_of(ref_random_d(&b, (double)lo, (double)hi))) {
                check(0, "global rand_range bits", SEEDS[s], i); break;
            }
            if (!same_state(&a, &b)) { check(0, "global bits state", SEEDS[s], i); break; }
        }
    }

    /* randi_range / %n reduction / Array.shuffle / pick_random. */
    for (int s = 0; s < NSEEDS; s++) {
        BtRng a; RefPcg b;
        bt_rng_seed(&a, SEEDS[s]); ref_seed(&b, SEEDS[s]);
        static const int RANGES[][2] = {{-30, 30}, {10, 2}, {7, 7}, {0, 0}, {-5, -5}, {0, 100}, {-1000, 1000}};
        for (int i = 0; i < 400; i++) {
            const int lo = RANGES[i % 7][0], hi = RANGES[i % 7][1];
            if (bt_rng_randi_range(&a, lo, hi) != ref_randi_range(&b, lo, hi) || !same_state(&a, &b)) {
                check(0, "randi_range", SEEDS[s], i); break;
            }
        }
        static const unsigned int BOUNDS[] = {1u, 2u, 3u, 4u, 61u, 100u, 255u, 1000u, 0xFFFFFFFFu};
        for (int i = 0; i < 400; i++) {
            const unsigned int n = BOUNDS[i % 9];
            if (bt_rng_bounded(&a, n) != ref_next(&b) % n || !same_state(&a, &b)) {
                check(0, "bounded %n", SEEDS[s], i); break;
            }
        }
        static const int SIZES[] = {0, 1, 2, 3, 5, 8, 64};
        for (int i = 0; i < 200; i++) {
            const int n = SIZES[i % 7];
            int x[64], y[64];
            for (int k = 0; k < 64; k++) x[k] = y[k] = k;
            ref_shuffle(&b, x, n);
            for (int k = n - 1; k >= 1; k--) {
                const int j = (int)bt_rng_bounded(&a, (unsigned int)(k + 1));
                const int t = y[j]; y[j] = y[k]; y[k] = t;
            }
            if (memcmp(x, y, sizeof(x)) || !same_state(&a, &b)) {
                check(0, "Array::shuffle", SEEDS[s], i); break;
            }
            const int size = (n == 0) ? 1 : n;
            if (bt_rng_bounded(&a, (unsigned int)size) != (unsigned int)ref_pick_random(&b, size) ||
                !same_state(&a, &b)) {
                check(0, "Array::pick_random", SEEDS[s], i); break;
            }
        }
    }

    /* Forced proto_exp_offset == 0 (pcg.cpp output of old state 0 is 0). */
    {
        BtRng a; RefPcg b;
        bt_rng_seed(&a, 12345); ref_seed(&b, 12345);
        a.state = 0; b.state = 0;
        float zf = bt_rng_randf(&a);
        check(zf == 0.0f && f32_eq(zf, ref_randf(&b)) && same_state(&a, &b), "randf zero short circuit", 12345, -1);
        a.state = 0; b.state = 0;
        double zd = (double)bt_rng_randd(&a);
        check(zd == 0.0 && f64_eq(zd, ref_randd(&b)) && same_state(&a, &b), "randd zero short circuit", 12345, -1);
        a.state = 0; b.state = 0;
        check(bt_math_randf(&a) == (float)ref_next(&b) / (float)0xFFFFFFFF && same_state(&a, &b),
              "randf zero state advance", 12345, -1);
        for (int i = 0; i < 500; i++) {
            if (!f32_eq(bt_rng_randf(&a), ref_randf(&b)) || !same_state(&a, &b)) {
                check(0, "post short-circuit stream", 12345, i); break;
            }
        }
    }

    /* Script-member binary64 helpers (brotato_rng.h bt_rng_f64_*). */
    static const double ADD[][2] = {{1.5, 300.0}, {-1e-9, 300.0}, {3.141592653589793, 300.0}, {2.5, -300.0}};
    for (int i = 0; i < 4; i++) {
        const double sum = ADD[i][0] + ADD[i][1];
        check(bt_rng_f64_add_bits(bits_of(ADD[i][0]), bits_of(ADD[i][1])) == bits_of(sum),
              "f64 add bits", i, -1);
        check(bt_rng_f64_trunc_bits(bits_of(sum)) == bits_of((double)(int)sum),
              "f64 trunc bits", i, -1);
    }

    if (fails == 0) puts("ok");
    return fails ? 1 : 0;
}
''')
    binary = tmp_path / "engine_formulas"
    subprocess.run(
        [os.environ.get("CC", "clang"), "-std=c11", "-O2", "-ffp-contract=off",
         "-I", str(ROOT), str(source), "-o", str(binary), "-lm"],
        check=True, capture_output=True, text=True,
    )
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    assert result.returncode == 0, result.stdout + result.stderr
    assert result.stdout.strip() == "ok", result.stdout

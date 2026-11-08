// Capacity-latch trap test: a port bound that rejects work the source accepts
// must FAIL CLOSED on both backends, and the CUDA path fails closed by TRAPPING
// the kernel (bt_gpu_cap_latch -> __trap()), so it cannot be observed from
// inside the process that triggers it.  This test therefore:
//
//   * builds with a deliberately tiny port bound (-DBT_MAX_PROJ=4; the caps are
//     #ifndef-guarded in brotato_state.h, and the #error below refuses a build
//     with the production bound, so a PASS can never be an artefact of the real
//     bounds being big enough);
//   * checks host-side that the scenario really does exhaust the bound and that
//     the class named is PROJ (the enemy-projectile pool);
//   * forks a child for the HOST wrapper's fail-closed path
//     (bt_capacity_require_clean aborts) and a second child for the DEVICE path
//     (puf_step must die with the capacity message well before its step budget);
//     the parent never calls CUDA, because forking a process that already owns a
//     CUDA context is not supported -- each child does its own backend's work.
//
// Compile (from the repository root, sm_120 = RTX 5070):
//   nvcc -O2 -std=c++17 -arch=sm_120 --fmad=false -Xcompiler=-ffp-contract=off \
//     -Xcompiler=-Wno-narrowing -Xcompiler=-Wno-unreachable-code \
//     --diag-suppress=2361 --diag-suppress=111 --diag-suppress=128 \
//     -DBT_MAX_PROJ=4 \
//     -I. -Isrc -Iocean/brotato -Ivendor -Iraylib-5.5_linux_amd64/include \
//     tests/test_brotato_capacity.cu raylib-5.5_linux_amd64/lib/libraylib.a \
//     -lm -Xlinker=-lpthread -lGL -o /tmp/test_brotato_capacity
//   /tmp/test_brotato_capacity          # exit 0 = both paths failed closed

#include <cuda_runtime.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <signal.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "../ocean/brotato/brotato.cu"

// Everything below is HOST-ONLY: the wrapper's fail-closed diagnostic lives in
// brotato_capacity_host.h, whose declarations are wrapped in BT_HOST_ONLY (they
// vanish in a device pass, which would leave stray function bodies behind), and
// the test itself forks children rather than running any kernel of its own.  The
// device pass therefore skips the whole block; the host pass of this same
// translation unit sees it.  (brotato.h itself cannot be included: it defines the
// CPU wrapper's puf_init/puf_reset/puf_step, which brotato.cu already defined.)
#if !defined(__CUDA_ARCH__)
#include "../ocean/brotato/brotato_capacity_host.h"

#if BT_MAX_PROJ > 8
#error "build tests/test_brotato_capacity.cu with -DBT_MAX_PROJ=<=8: this test needs a bound the scenario really exhausts"
#endif

#define TRAP_STEP_BUDGET 5000   // the loss lands at ~tick 513, so a trap is far inside this

static void ck(cudaError_t err, const char *what) {
    if (err != cudaSuccess) {
        std::fprintf(stderr, "CUDA error (%s): %s\n", what, cudaGetErrorString(err));
        std::fflush(stderr);
        _exit(70);
    }
}

static int action_for(int step, int env) {
    unsigned int h = (unsigned int)step * 2654435761u ^ (unsigned int)(env + 1) * 40503u;
    h ^= h >> 13;
    h *= 2246822519u;
    h ^= h >> 16;
    return (int)(h % (unsigned int)BT_ACTIONS);
}

// The scenario the tiny bound cannot serve: a dense wave-20 endless arena with a
// projectile-firing weapon.  Same one the CPU probe used to pick the bound.
static void init_dense(BtState *s) {
    bt_core_init(s, 1ull, 0, 1, BT_CHARACTER_DEMON,
                 (unsigned short)bt_weapon_id_from_name("weapon_ghost_scepter_1"), 1);
    s->endless = 1;
    s->start_wave = 20;
    bt_core_reset(s);   // re-enter wave 20 (start_wave is honoured by bt_enter_wave)
}

static int failures = 0;
static void check(bool ok, const char *what) {
    if (!ok) { failures++; std::printf("  FAIL: %s\n", what); }
    std::fflush(stdout);
}

// ---------------------------------------------------------------------------
// Child bodies.  They run in a forked process so a trap/abort is observable.
// ---------------------------------------------------------------------------

// HOST fail-closed: the CPU wrapper's diagnostic must abort, not return.
static void child_host_require_clean(void) {
    static BtState s;
    float obs[BT_OBS_SIZE], rew, term;
    init_dense(&s);
    for (int t = 0; t < 1000; t++) bt_core_step(&s, action_for(t, 0), obs, &rew, &term);
    char buf[192];
    std::fprintf(stderr, "child: mask before require_clean: %s\n", bt_capacity_describe(&s, buf, sizeof buf));
    std::fflush(stderr);
    bt_capacity_require_clean(&s, "test_brotato_capacity/child_host_require_clean");
    std::fprintf(stderr, "child: require_clean RETURNED on a faulted state\n");
    std::fflush(stderr);
    _exit(3);   // reaching here is the failure this child exists to detect
}

// DEVICE fail-closed: puf_step must trap the kernel (device printf + sticky CUDA
// error) instead of returning a truncated rollout.
static void child_device_trap(void) {
    const int n = 2;
    obs_t *obs_d = nullptr;
    float *act_d = nullptr, *rew_d = nullptr, *term_d = nullptr;
    ck(cudaMalloc(&obs_d, (size_t)n * BT_OBS_SIZE * sizeof(obs_t)), "cudaMalloc obs");
    ck(cudaMalloc(&act_d, (size_t)n * sizeof(float)), "cudaMalloc actions");
    ck(cudaMalloc(&rew_d, (size_t)n * sizeof(float)), "cudaMalloc rewards");
    ck(cudaMalloc(&term_d, (size_t)n * sizeof(float)), "cudaMalloc terminals");
    Dict *kw = (Dict *)std::calloc(1, sizeof(Dict));
    kw->name = dict_strdup("test_brotato_capacity");
    dict_set(kw, "zone", 0.0);
    dict_set(kw, "shop_steps", 1.0);
    dict_set(kw, "endless", 1.0);
    dict_set(kw, "start_wave", 20.0);
    dict_set_str(kw, "character", "demon");
    dict_set_str(kw, "starting_weapon", "weapon_ghost_scepter_1");
    Env *envs = puf_vec_create(n, kw, obs_d, act_d, rew_d, term_d);
    if (envs == nullptr) { std::fprintf(stderr, "child: puf_vec_create failed\n"); _exit(4); }
    puf_reset(envs);
    ck(cudaDeviceSynchronize(), "child reset sync");
    std::vector<float> acts(n, 0.0f);
    for (int t = 0; t < TRAP_STEP_BUDGET; t++) {
        if (t % 200 == 0) { std::fprintf(stderr, "child: step %d\n", t); std::fflush(stderr); }
        for (int i = 0; i < n; i++) acts[i] = (float)action_for(t, i);
        ck(cudaMemcpy(act_d, acts.data(), (size_t)n * sizeof(float), cudaMemcpyHostToDevice), "H2D acts");
        puf_step(envs);
        ck(cudaDeviceSynchronize(), "child step sync");   // surfaces the trap's sticky error
    }
    std::fprintf(stderr, "child: NO TRAP after %d steps (the bound was not exhausted?)\n", TRAP_STEP_BUDGET);
    std::fflush(stderr);
    _exit(5);   // reaching here is the failure this child exists to detect
}

// Run `fn` in a forked child, capturing stdout+stderr; return the captured text
// and the raw wait() status.
static std::string fork_capture(void (*fn)(void), int *status_out) {
    int fds[2];
    if (pipe(fds) != 0) { std::perror("pipe"); std::exit(1); }
    pid_t pid = fork();
    if (pid < 0) { std::perror("fork"); std::exit(1); }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[1]);
        setvbuf(stdout, nullptr, _IONBF, 0);
        fn();            // no return expected
        _exit(99);
    }
    close(fds[1]);
    std::string out;
    char buf[4096];
    ssize_t r;
    while ((r = read(fds[0], buf, sizeof buf)) > 0) out.append(buf, (size_t)r);
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    if (status_out) *status_out = st;
    return out;
}

static bool has(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
int main(void) {
    std::printf("capacity test: BT_MAX_PROJ=%d (production bound is 1024; this build is deliberately small)\n",
                BT_MAX_PROJ);

    // ---- 1. host-side: the control configuration must stay clean ---------- */
    {
        static BtState s;
        float obs[BT_OBS_SIZE], rew, term;
        bt_core_init(&s, 7ull, 0, 1, BT_CHARACTER_BULL, BT_WEAPON_NONE, 1);
        for (int t = 0; t < 1000; t++) bt_core_step(&s, action_for(t, 0), obs, &rew, &term);
        unsigned long long m = bt_capacity_mask(&s);
        char buf[192];
        bt_capacity_describe(&s, buf, sizeof buf);
        std::printf("host control (bull+none wave 1, 1000 ticks): %s\n", buf);
        check(m == 0ull, "the control configuration must not show a capacity loss");
    }

    // ---- 2. host-side: the forced overflow is detected and classified ----- */
    {
        static BtState s;
        float obs[BT_OBS_SIZE], rew, term;
        init_dense(&s);
        for (int t = 0; t < 1000; t++) bt_core_step(&s, action_for(t, 0), obs, &rew, &term);
        unsigned long long m = bt_capacity_mask(&s);
        char buf[192];
        bt_capacity_describe(&s, buf, sizeof buf);
        std::printf("host forced overflow (demon+scepter endless wave 20, 1000 ticks): %s\n", buf);
        check(m != 0ull, "the forced overflow must set a capacity loss");
        check((m & (1ull << BT_CAP_PROJ)) != 0ull, "the loss must be classified as PROJ (the tiny table)");
    }

    // ---- 3. host wrapper: bt_capacity_require_clean must abort ----------- */
    {
        int st = 0;
        std::string out = fork_capture(child_host_require_clean, &st);
        bool aborted = WIFSIGNALED(st) ? (WTERMSIG(st) == SIGABRT) : (WEXITSTATUS(st) != 0);
        std::printf("host fail-closed child: %s, signaled=%d sig=%d exit=%d\n", out.c_str(),
                    (int)WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : 0,
                    WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        check(aborted, "bt_capacity_require_clean must abort on a faulted state");
        check(has(out, "IMPLEMENTATION CAPACITY LOSS"), "the host diagnostic must name the loss");
        check(has(out, "PROJ"), "the host diagnostic must name the exhausted class (PROJ)");
        check(!has(out, "require_clean RETURNED"), "bt_capacity_require_clean must not return");
    }

    // ---- 4. device: puf_step must trap the kernel ------------------------ */
    {
        int st = 0;
        std::string out = fork_capture(child_device_trap, &st);
        // Highest "child: step N" marker the child reached, to prove it died early.
        int last = -1;
        for (size_t p = 0; (p = out.find("child: step ", p)) != std::string::npos; p += 12)
            last = std::atoi(out.c_str() + p + 12);
        std::printf("device trap child: %s", out.c_str());
        if (out.empty() || out[out.size() - 1] != '\n') std::printf("\n");
        std::printf("                    last progress marker: step %d (budget %d), signaled=%d sig=%d exit=%d\n",
                    last, TRAP_STEP_BUDGET, (int)WIFSIGNALED(st), WIFSIGNALED(st) ? WTERMSIG(st) : 0,
                    WIFEXITED(st) ? WEXITSTATUS(st) : -1);
        check(WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) != 0),
              "the device child must die (the kernel traps, so there is no clean return)");
        // The device printf can be lost when the kernel traps (the context is
        // poisoned), so accept the loss text OR the CUDA error the trap raises;
        // the tick the child reached is the strong evidence that the tiny bound
        // (not something else) killed it.
        check(has(out, "capacity loss") || has(out, "CUDA error"),
              "the device failure must be the capacity loss (its message or the CUDA error it raises)");
        check(!has(out, "NO TRAP"), "puf_step must not survive the exhausted bound");
        check(last >= 0 && last < 1000, "the trap must land near the predicted tick, not at the budget");
    }

    std::printf("%s: %d failures\n", failures == 0 ? "PASS" : "FAIL", failures);
    return failures == 0 ? 0 : 1;
}
#endif  /* !__CUDA_ARCH__ */

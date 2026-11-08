// Replay tick/action pairs from stdin through the actual shared CPU core.
// Tick 0 is the initial hold snapshot; tick N>0 records transition N's action.
//
// Initialization interface (the profile must match the run being replayed):
//   core_trace [seed [zone [character [starting_weapon [obs_size [wave_duration
//                          [start_wave [audio_rng [particles_rng [invulnerable [endless]]]]]]]]]]] < tick_actions.txt
// `character` / `starting_weapon` are canonical generated-table ids
// (brotato_characters.h BT_CHARACTERS order / brotato_weapons.h BT_WEAPONS
// order); the defaults reproduce the historical weaponless Bull run
// (BT_CHARACTER_BULL, BT_WEAPON_NONE).  replay_cpu.py resolves them from the
// source trace's recorded profile, so a Knight/Demon/weapon run is never
// silently replayed as Bull.
//
// `obs_size` (0 = the compiled BT_OBS_SIZE) truncates the emitted observation to
// the width the source trace declares: the v4 core appends blocks (BT_OBS_TARGET)
// after the v3 161 fields, so a v3 trace is replayed by emitting its own prefix.
// `wave_duration` (> 0) replaces every wave's generated duration with the value
// the trace recorded in its wave_duration column (the bridge's instant_waves
// debug knob records 1-6 s waves); 0 keeps the generated table.
//
// `start_wave` (> 0) is the trace's recorded first wave (the bridge's
// starting_wave knob); 0/1 keep the historical wave-1 boot.
//
// `audio_rng` (0/1) is the trace's recorded audio-RNG model: 1 (default) keeps
// the legacy behaviour where the sim consumes the source's sound draws, 0
// selects the bridge's audiovisual neutralization (its sound queues/arrays are
// cleared, so exactly those draws do not happen).  See BtState.audio_rng.
//
// `invulnerable` (0/1) is the trace's recorded DebugService.invulnerable
// (RunData.invulnerable): 0 (default) keeps the normal player hurtbox, 1
// disables it so no enemy contact/projectile reports the player.  See
// BtState.invulnerable.
//
// `endless` (0/1) is the trace's recorded RunData.is_endless_run: 0 (default)
// keeps the campaign (a wave-20 clear ends the episode), 1 continues to wave 21+
// (see BtState.endless).  Without it an endless recording replays as a campaign
// run that ends -- and bt_core_step resets -- at the wave-20 clear.
#include "../ocean/brotato/brotato_core.h"
#include <cstdio>
#include <cstdlib>
static BtState state;
int main(int argc, char **argv) {
    if (argc > 12) {
        /* argc misuse must be loud on BOTH streams: an older harness with a
         * tighter guard silently produced an EMPTY candidate trace (a
         * comparator can read that as "every row missing"), which the team has
         * mistaken for a real divergence more than once.  `ERROR argv` on stderr
         * plus a zero-row marker on stdout (a comment line, so a JSONL reader
         * fails loudly instead of seeing silence) makes the cause unambiguous. */
        std::fprintf(stderr,
            "ERROR argv: too many arguments (%d)\n"
            "usage: core_trace [seed [zone [character [starting_weapon [obs_size [wave_duration"
            " [start_wave [audio_rng [particles_rng [invulnerable [endless]]]]]]]]]] < tick_actions.txt\n", argc - 1);
        std::printf("# ERROR argv: refusing to emit a trace (usage above; %d arguments)\n",
                    argc - 1);
        return 2;
    }
    const unsigned long long seed = argc > 1 ? std::strtoull(argv[1],0,10) : 1;
    const int zone = argc > 2 ? std::atoi(argv[2]) : 0;
    const long character = argc > 3 ? std::strtol(argv[3],0,10) : (long)BT_CHARACTER_BULL;
    const long weapon = argc > 4 ? std::strtol(argv[4],0,10) : (long)BT_WEAPON_NONE;
    const long obs_size = argc > 5 ? std::strtol(argv[5],0,10) : 0;
    const double wave_duration = argc > 6 ? std::strtod(argv[6],0) : 0.0;
    /* Wave the replay starts from (1 = the historical wave-1 recording).  An
     * endless recording's first row is wave 20+, and the trace's recorded wave
     * is forwarded here by replay_cpu.py as a trailing argument. */
    const long start_wave = argc > 7 ? std::strtol(argv[7],0,10) : 0;
    /* Audio-RNG model the recording used: 1 (default) = the sim consumes the
     * source's sound draws, 0 = the bridge neutralized them. */
    const long audio_rng = argc > 8 ? std::strtol(argv[8],0,10) : 1;
    /* Particle-RNG model the recording used: 1 (default) = the sim consumes the
     * source's CPUParticles2D draws, 0 = the bridge suppressed them. */
    const long particles_rng = argc > 9 ? std::strtol(argv[9],0,10) : 1;
    /* DebugService.invulnerable the recording used: 0 (default) = the normal
     * player hurtbox, 1 = the player Hurtbox disabled (no enemy contact or
     * projectile hit is registered). */
    const long invulnerable = argc > 10 ? std::strtol(argv[10],0,10) : 0;
    /* RunData.is_endless_run the recording used: 0 (default) = the campaign, so
     * a wave-20 clear ends the episode; 1 = the endless run (waves 21+). */
    const long endless = argc > 11 ? std::strtol(argv[11],0,10) : 0;
    if (!((character >= 0 && character < (long)BT_NCHARACTERS) || character == (long)BT_CHARACTER_NONE)) {
        std::fprintf(stderr,"character id %ld is outside 0..%u (or BT_CHARACTER_NONE)\n",
            character, (unsigned)BT_NCHARACTERS - 1u);
        return 2;
    }
    if (!((weapon >= 0 && weapon < (long)BT_NWEAPONS) || weapon == (long)BT_WEAPON_NONE)) {
        std::fprintf(stderr,"weapon id %ld is outside 0..%d (or BT_WEAPON_NONE)\n",
            weapon, (int)BT_NWEAPONS - 1);
        return 2;
    }
    if (obs_size < 0 || obs_size > (long)BT_OBS_SIZE) {
        std::fprintf(stderr,"obs_size %ld is outside 0..%u\n", obs_size, (unsigned)BT_OBS_SIZE);
        return 2;
    }
    const int out_size = obs_size > 0 ? (int)obs_size : (int)BT_OBS_SIZE;
    /* shop_steps 0: the SOURCE's post-purchase buy delay (0.05 s = 4 frames,
     * shop_items_container.gd:27-28 / bt_timer_frames), not a short one. */
    bt_core_init(&state,seed,zone,0, (unsigned short)character, (unsigned short)weapon,
                 (unsigned char)(audio_rng ? 1 : 0));
    /* Set after init and before the wave reset below: bt_core_reset preserves
     * the option, and nothing in the run prologue draws a particle. */
    state.particles_rng = (unsigned char)(particles_rng ? 1 : 0);
    /* DebugService.invulnerable, recorded in the trace metadata; set before the
     * wave reset below so bt_core_reset preserves it (like particles_rng). */
    state.invulnerable = (unsigned char)(invulnerable ? 1 : 0);
    /* RunData.is_endless_run, also set before the wave reset below so the
     * re-run prologue and the first wave build see it (bt_core_reset leaves it,
     * mirroring brotato.h puf_init). */
    state.endless = (unsigned char)(endless ? 1 : 0);
    if (wave_duration > 0.0 || start_wave > 1) {
        /* Re-run the deterministic reset on FRESH streams (clearing rng_ready
         * makes it replay the run prologue instead of appending a second one),
         * with the trace's recorded wave duration and, for a recording that does
         * not start at wave 1, the trace's own starting wave.  bt_core_reset
         * enters BtState.start_wave (RunData.reset() `current_wave =
         * DebugService.starting_wave`, run_data.gd:549), so an endless take
         * enters its wave directly with no catch-up draws.  A wave-1 recording
         * keeps the historical call shape. */
        if (wave_duration > 0.0) state.wave_ovr = (float)wave_duration;
        if (start_wave > 1) state.start_wave = (unsigned short)start_wave;
        state.rng_ready = 0;
        bt_core_reset(&state);
    }
    int tick, action, previous = -1;
    float obs[BT_OBS_SIZE], reward = 0.0f, terminal = 0.0f;
    while (std::scanf("%d %d",&tick,&action)==2) {
        if (tick != previous + 1 || action < 0 || action >= BT_ACTIONS ||
            (tick == 0 && action != 0)) {
            std::fprintf(stderr,"invalid tick/action pair: %d %d\n",tick,action); return 2;
        }
        previous=tick;
        if (tick == 0) bt_core_observe(&state, obs);
        else bt_core_step(&state,action,obs,&reward,&terminal);
        int enemy_nodes = 0;   /* observation count: live + death-animation nodes */
        for (int e = 0; e < BT_MAX; e++) enemy_nodes += state.ealive[e] || state.edying[e] != 0;
        std::printf("{\"tick\":%d,\"action\":%d,\"hp\":%.9g,\"wave\":%d,\"enemies\":%d,\"enemy_nodes\":%d,\"x\":%.9g,\"y\":%.9g,\"reward\":%.9g,\"terminal\":%.9g,\"obs\":[",
            /* `hp` is the BRIDGE's field: RunData.get_player_current_health(0),
             * which is players_data.current_health only while a wave is in
             * progress and the stat-layer max otherwise (run_data.gd; the same
             * rule bt_core_observe uses for obs[0]).  RunData.wave_in_progress
             * stays set through the whole wave-end window and clears at the shop
             * transition (the traced wave_in_progress column is 1 on the window
             * rows and 0 from the shop on), so the window still reports
             * state.hp and only phase 1/2 report state.max_hp -- which is where
             * a Demon purchase's stat_max_hp payment shows up. */
            tick,action,state.phase == 0 ? state.hp : state.max_hp,
            state.wave,state.ecount,enemy_nodes,state.px,state.py,reward,terminal);
        for (int i=0;i<out_size;i++) std::printf("%s%.9g",i?",":"",obs[i]);
        /* The sim's own legality row for this state, so a capture's per-row
         * bridge mask can be asserted equal to it (the end-to-end mask check:
         * replay output carries the sim mask, trace rows carry the bridge's
         * applied mask).  Printed as ints, one per action id. */
        {
            unsigned char m[BT_ACTIONS];
            bt_action_mask(&state, m);
            std::printf("],\"mask\":[");
            for (int i=0;i<BT_ACTIONS;i++) std::printf("%s%d",i?",":"",m[i]?1:0);
        }
        std::puts("]}");
    }
    if (!std::feof(stdin)) { std::fprintf(stderr,"malformed input pair\n"); return 2; }
    return 0;
}

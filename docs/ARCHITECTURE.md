# Nexus Resonance Architecture

## Overview

Nexus Resonance is a Godot 4 GDExtension for spatial audio using Steam Audio (Phonon). This document describes the C++ architecture, threading model, and critical synchronization.

## Module Structure

```
register_types.cpp     → Module init/uninit, class registration
ResonanceServer       → Central singleton: Steam Audio context, scene, simulator
ResonanceBaker        → Probe baking (reflections, pathing, static endpoints)
ResonanceSceneManager → Scene export, asset loading, OBJ/MTL
ProbeBatchRegistry    → Probe batch handle management, hash deduplication
HandleManagerBase     → Source/batch handle allocation (overflow-safe)
```



## Scene node surface (GDExtension)

Registered in `register_types.cpp` for the Create Node dialog (no attached GDScript):


| Node                                                                         | Base                  | Role                                                                                                                                              |
| ---------------------------------------------------------------------------- | --------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| `ResonanceRuntime`                                                           | `Node`                | Native runtime orchestrator (`src/resonance_runtime*.cpp`); drives GDScript helpers; see [ADR-001](adr/001-native-resonance-node-migration.md)    |
| `ResonanceListener`                                                          | `Node3D`              | Listener pose → `ResonanceServer`                                                                                                                 |
| `ResonancePlayer`                                                            | `AudioStreamPlayer3D` | Spatial source + internal stream (Steam params on `ResonancePlayerConfig`; ASP3D spatial knobs hidden when config set)                            |
| `ResonanceAmbisonicPlayer`                                                   | `AudioStreamPlayer`   | HOA bed decode (`apply_output_gain` / `kAmbisonicDecoderOutputScalar` on the bed path only; mixer wet Ambisonics decode keeps Steam Audio levels) |
| `ResonanceFmodEventEmitter`                                                  | `Node3D`              | Child of `FmodEventEmitter3D`; FMOD bridge source sync                                                                                            |
| `ResonanceCodaEventEmitter`                                                  | `Node3D`              | Coda event playback + `ResonanceCodaBridge` spatial link                                                                                          |
| `ResonanceProbeVolume`                                                       | `Node3D`              | Probes / bake volume                                                                                                                              |
| `ResonanceStaticScene`                                                       | `Node3D`              | Static scene asset                                                                                                                                |
| `ResonanceGeometry` / `ResonanceStaticGeometry` / `ResonanceDynamicGeometry` | `Node3D`              | Export / runtime geometry                                                                                                                         |


Implementation: `src/resonance_fmod_event_emitter.cpp`, `src/resonance_coda_event_emitter.cpp` (ADR-001 Phase 1.1/1.2).

Public GDScript helpers (not native nodes): `ResonanceRuntimeBaker` (RAM probe bake) and `ResonanceRuntimeExporter` (play-mode static rebuild). Class reference: `addons/nexus_resonance/doc_classes/`.

## ResonancePlayer ownership (ASP3D + Hide)

`ResonancePlayer` keeps `AudioStreamPlayer3D` as the transport base (stream API, volume/pitch, polyphony, transform). Steam Audio parameters live on `ResonancePlayerConfig`. With a config assigned, inert Godot spatial properties are hidden in the inspector and Godot attenuation is forced off (`ATTENUATION_DISABLED`, Godot `max_distance` cleared) so only Steam distance/occlusion/HRTF apply. Godot has no spatializer plugin hook, so host playback and Steam Source share one node via inheritance + config. A bare `Node3D` rebase is **not** the architecture: Phonon does not own playback.

## Editor vs runtime Steam Audio init

`ResonanceRuntime::initialize_server()` returns immediately in the editor (`Engine::is_editor_hint()`). Probe baking uses its own server setup; scene preview does **not** start the gameplay audio engine. `ResonanceAmbisonicInternalPlayback::prewarm_steam_audio()` therefore stays uninitialized in the editor: HOA beds W-passthrough (omni W channel only) until a running game inits `ResonanceServer`. This is expected for inspector audition, not a bug.

## Initialization Flow

1. `initialize_nexus_resonance_module(MODULE_INITIALIZATION_LEVEL_SCENE)` registers all classes
2. `ResonanceServer` singleton is created and registered
3. `ResonanceSteamAudioContext::init()` creates IPL context, HRTF, Embree/OpenCL/TAN
4. `_init_scene_and_simulator()` creates IPLScene, IPLSimulator, ReflectionMixer
5. Worker thread starts for the simulation tick loop when `scene_type` is not **Custom** (`IPL_SCENETYPE_CUSTOM`). With Custom (Godot Physics), Phonon runs on the main/physics thread inside `ResonanceServer::tick()` and no worker is started (see **Custom scene threading** below).



## Shutdown Order (Critical)

1. `uninitialize_nexus_resonance_module` calls `ResonanceServer::shutdown()`
2. Worker thread joined, `thread_running = false`
3. Steam Audio resources released: mixer → simulator → scene → context
4. Singleton unregistered and deleted

**Do not** call `ResourceSaver.remove_resource_format_saver` / `ResourceLoader.remove_resource_format_loader` in `_exit_tree`; Godot may tear down before plugin exit, causing SIGSEGV.

## Lock Order (Mutex Hierarchy)

IPL scene/simulator work and the worker phonon tick are serialized by `simulation_mutex`. Other mutexes are domain-specific; avoid holding `simulation_mutex` while waiting on `AudioServer::lock`.

When both registry and simulation state are needed:

1. `simulation_mutex` (scene commit, sources, bake, `_run_phonon_simulation_locked`)
2. `probe_batch_registry_.mutex_` (load/remove/revalidate probe batches)

The worker already holds `simulation_mutex` when it calls `get_pathing_batch` / `for_each_probe_data` (which take `mutex_`). Taking `mutex_` first then `simulation_mutex` deadlocks that path (`resonance_probe_batch_registry.cpp`).

Separate (do not nest with `simulation_mutex` unless a call site documents it): `pending_source_lifecycle_mutex_`, `worker_mutex`, `pathing_vis_mutex`, `_attenuation_callback_mutex`, `dynamic_instanced_transform_queue_mutex_`, `ipl_context_clients_mutex_`.

## Custom scene threading (`IPL_SCENETYPE_CUSTOM`)

Steam Audio recommends running **RunReflections** and **RunPathing** off the audio thread, and **RunDirect** (with occlusion/transmission) off the audio thread as well. Nexus Resonance normally does this on a dedicated worker when the built-in tracer is used.

**Custom** (Godot Physics ray tracer) is an intentional tradeoff: Phonon simulation runs on the **main/physics thread** inside `ResonanceServer::tick()` under `simulation_mutex` (`_uses_main_thread_phonon_simulation()`), and **no worker thread** is started. Reasons:

- `IPL_SCENETYPE_CUSTOM` trace callbacks call Godot `intersect_ray`, which must run on the main thread (or a physics thread that shares the same world state).
- Moving Custom simulation to a background worker would require a safe cross-thread physics query path that Godot does not provide without stale-world risk.

Implications: high `max_rays` or pathing on Custom can hitch frames; `ResonanceRuntime::warn_custom_tracer_main_thread_sim` logs once at runtime. Prefer Default/Embree for heavy realtime sim; disable `physics/3d/run_on_separate_thread` when using Custom (see `ResonanceRuntimeConfig` docs). A full worker rewrite for Custom is out of scope unless Godot exposes thread-safe physics queries.

## Thread Contexts


| Context                         | Code                                                                                                                                                                                                                                                                                                                                                                        |
| ------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Main thread                     | `update_source`, `load_probe_batch`, `tick` (Custom: `_run_phonon_simulation_locked`), probe/volume/player updates, `prewarm_steam_audio`, `ResonanceAudioEffectInstance::try_prewarm_processor`                                                                                                                                                                            |
| Audio thread                    | `fetch_reverb_params`, `fetch_pathing_params`, `get_source_occlusion_data` (epoch caches only), `ResonanceStreamPlayback::_mix`, `ResonanceAudioEffectInstance::_process`. No `ipl*EffectCreate` / buffer alloc in `_mix` - dry passthrough until main-thread `prewarm_steam_audio` / `try_prewarm_processor`.                                                              |
| Worker thread                   | `_worker_thread_func`, `iplSimulatorRunDirect`, `RunReflections`, `RunPathing`, `_worker_sync_fetch_caches` (`iplSourceGetOutputs`) — **not used for Custom scene type**                                                                                                                                                                                                    |
| Bake thread (GDScript `Thread`) | `ResonanceServer::bake_*` via `_with_bake_scene`: Phonon bake on an **isolated temp** `IPLScene` (asset / runtime-static clone / live snapshot). `simulation_mutex` only during prepare. Progress via `get_bake_progress()` atomic poll - not Godot signals. Stop editor play mode before baking or export (`is_playing_scene()` guards on bake runner and export handler). |
| Callbacks                       | `_pathing_vis_callback`, `_distance_attenuation_callback` run in worker/simulation context                                                                                                                                                                                                                                                                                  |




## Lock-Free Audio Paths

- **Simulation outputs**: Worker fills occlusion/reflection/pathing double buffers under `simulation_mutex`; audio reads published front slots by epoch (no `simulation_mutex` on fetch).
- **Reflection mixer**: `std::atomic<IPLReflectionMixer>` plus `MixerReadGuard` (reader count). Swaps/releases run on main/worker only.
- **Listener pose**: Seqlock on `listener_coords_latest_` / `listener_seq_` (`get_current_listener_coords`).
- **Spatial output gate**: `global_triangle_count` (atomic), `phonon_scene_audio_ready_`, `spatial_audio_warmup_passes_remaining_` (`is_spatial_audio_output_ready`).
- **ResonancePlayer polyphony**: Main mutates `internal_playbacks_` under mutex, then publishes `playback_snap_[2]`; `ResonanceReverbPlayback::_mix` reads the snapshot without locking.
- **ResonanceStreamPlayback source**: `update_parameters` (main) retains `IPLSource` into atomics; `_sync_params` (audio) only copies the handle and reads simulation caches, never `SourceManager::mutex`. On tree exit, `ResonancePlayer` pushes `source_handle = -1` to all playbacks before `destroy_source_handle`.
- **Logging**: `ResonanceLog` posts to a lock-free ring from non-main threads; `ResonanceServer::tick` drains to Godot on the main thread.



## Spatial output startup gate

Spatial output is muted only on **cold start / full scene reload / first triangles into an empty scene**, not on incremental geometry edits (e.g. spawning `ResonanceDynamicGeometry` into an already non-empty scene).

- `notify_geometry_changed` with a non-zero triangle delta updates `global_triangle_count` and marks the scene dirty. It calls `arm_spatial_audio_output_gate()` only when the count crosses **0 -> N** (`spatial_audio_geometry_notify_should_arm_gate` in `resonance_constants.h`).
- `arm_spatial_audio_output_gate()` clears `phonon_scene_audio_ready_` and resets `spatial_audio_warmup_passes_remaining_` to `kSpatialAudioWarmupWorkerPasses`. Explicit callers: runtime init/handoff/reinit, deferred refresh after `load_scene_data`, and hard static-pack clear/replace/load.

**Runtime config reinit:** IPL-context fields (`ambisonic_order`, HRTF toggles, SOFA index, frame size, etc.) go through `ResonanceRuntimeConfig.native_engine_reinit_requested` and `reload_after_reinit` (probe reload, source handles, bus prewarm). Until that frame completes, players may W-passthrough (`arm_spatial_audio_output_gate`). The reverb bus passes through its Godot bus chain until `ResonanceAudioEffectInstance` prewarm completes (no shared convolution wet mixed yet). Patching config without `ResonanceRuntimeConfig` setters (no signal) can still drift until manual reinit. SN3D normalization at bake time is not live-toggled.

**Reverb activator:** `ResonanceReverbActivator` pushes silence only onto `ResonanceRuntimeConfig.reverb_bus_name` so `ResonanceAudioEffect` runs on the shared convolution/TAN path. Per-player `ResonancePlayerConfig` custom reverb buses (Parametric/Hybrid split) are not activator-fed; use a `ResonanceReverbOutput` child when that bus must stay hot without a player on it.

**Debug HUD signal R:** With Convolution reflection type and the player debug overlay on, the HUD `R` column shows shared reverb bus RMS (`get_reverb_bus_output_rms_pre_gain`), labeled `R(bus)`, not per-source wet send. Parametric/Hybrid still show per-player reverb send in `R`.

- The worker sets `phonon_scene_audio_ready_` after `iplSceneCommit`. `is_spatial_audio_output_ready()` combines warmup and commit (`spatial_audio_geometry_gate_allows_output`); audio and main read it via atomics only.
- Mesh rebuilds inside `ResonanceGeometry::_create_meshes` use a silent clear plus a single net triangle notify so clear+create does not falsely look like 0 -> N.

Until ready, `ResonancePlayer` does not push playback parameters (`params_ever_synced_` stays false), and `ResonanceStreamPlayback::_process_steam_audio_block` zeros spatial output instead of running direct/reflection/pathing. This avoids audible unoccluded output before the Phonon scene matches registered meshes on cold start.

### Accepted residual: incremental mesh add

Incremental triangle adds into an **already non-empty** scene do **not** re-arm the spatial output gate. Audio stays unmuted while `scene_dirty` is true until the worker runs `iplSceneCommit` + `iplSimulatorSetScene`. Until that commit tick completes, direct/occlusion may still use the **previous** committed scene (typically one worker pass; longer if the worker is already mid-tick). After commit, the worker runs `RunDirect` on that same tick when sources need direct output (`phonon_worker_run_direct_after_scene_graph_commit`) so occlusion caches refresh without muting. **Reflection/pathing** can still reflect the previous scene until the next `RunReflections` / `RunPathing` pass; forcing heavy reflection on every incremental add was rejected (per-spawn cost, hybrid IR epoch coupling). Dynamic instanced meshes use the transform queue + `mark_scene_commit_pending`; expect the same brief stale window after add or fast motion.

### Accepted residual: Custom scene readiness

With `IPL_SCENETYPE_CUSTOM`, `ResonanceGeometry` is a no-op (`global_triangle_count` often stays 0). `is_spatial_audio_output_ready()` can open after warmup alone (`spatial_audio_geometry_gate_allows_output`). Godot physics world binding (`set_physics_world`) runs from `ResonanceRuntime` each frame and may lag the first runtime tick; the first frames after init can see empty occlusion until the Custom tracer has a valid `World3D`. No large custom-tracer rewrite planned.

## Runtime pathing and probe batches (QA audit)



### Prb-01: Pathing capability from IPL layer

`ProbeBatchRegistry::load_batch` sets `handle_has_pathing_` from `iplProbeBatchGetDataSize` on the PATHING/DYNAMIC layer (`resonance_probe_batch_pathing_policy.h`), not from `pathing_params_hash`. The hash remains an incremental-bake hint; a mismatch (hash set, layer missing) logs a warning.

### Prb-02: Probe bake entry point

Native `ResonanceProbeVolume.bake_probes()` / `bake_probes_with_floor_points()` were removed. Supported path: `ResonanceBakeRunner.run_bake([volume])` (inspector Bake, Tools menu, or `ResonanceRuntimeBaker` for RAM bakes).

### Prb-04: Pathing batch selection

`resolve_pathing_batch` (`resonance_pathing_batch_lookup_policy.h`) fail-closes when:

- `ResonancePlayer.pathing_probe_volume` points at a batch without a pathing layer
- Multiple pathing volumes are loaded and the source has no explicit `pathing_probe_volume`

A single loaded pathing volume without an explicit assignment still works, with a once-per-source warning that includes the node path. Assign `pathing_probe_volume` in multi-volume scenes.

### Bake-02: Editor bake vs live simulator

Bake uses an isolated temp `IPLScene`; `_with_bake_scene` holds `simulation_mutex` only for dirty-commit and `_prepare_bake_scene`, not during the long Phonon bake. For the bake duration it holds `phonon_context_exclusive_mutex_` (before `simulation_mutex` when both are taken) so the Phonon worker and main-thread Custom-scene sim cannot use the shared `IPLContext` concurrently. **Hardening:** `ResonanceBakeRunner.run_bake` and `ResonanceExportHandler` menu exports block when the editor is in play mode (`is_playing_scene()`), avoiding bake/export parallel to a running scene and edited-root mutation while runtime uses other packs.

### Bake-03: Bake vs runtime pathing visibility samples

Bake and runtime share `ResonanceRuntimeConfig` pathing visibility settings: `pathing_num_samples` (default 4), `pathing_vis_range` / `pathing_vis_radius` / `pathing_vis_threshold` (defaults 1000 / 1.0 / 0.1), and bake-only `pathing_path_range` (default 1000). The bake pipeline copies them into `bake_pathing_*` Phonon keys; runtime uses the vis* values for `IPLSimulationInputs` / `numVisSamples`.

### Bake-04: Mid-pipeline probe reload

The editor bake pipeline sets `set_bake_pipeline_active(true)` for its full duration. `reload_probe_batch` skips while active; `load_probe_batch` skips pathing hash validation during the pipeline (reflections rebake clears pathing hash before the pathing step). Final reload runs when the pipeline finishes.

### Path-02: Pathing gated by `output_reverb` (intentional)

Nexus treats `ResonanceServer.output_reverb` as the master wet-path switch: reflections **and pathing** require it (`resonance_server_sources.cpp`, `phonon_worker_policy.h`). This matches a single "reverb bus active" debug/runtime gate. Per-source `reflections_mix_level` / `pathing_mix_level` scale wet DSP only and do **not** clear IPL simulation flags when mix is 0. Inspector enable overrides and global `pathing_enabled` still gate simulation. Playback `enable_reverb` / `enable_direct` follow `output_`* the same way: Mix 0 still calls `iplReflectionEffectApply` / `iplPathEffectApply` with silence so Overlap-Save and path state drain.

### Pathing distance attenuation (Steam Audio contract)

Steam `PathSimulator::findPaths` puts distance into path SH via `distanceAttenuationModel.evaluate(distance)` — not into `eqCoeffs` (those are deviation only). Nexus ramps `pathing_mix_level` each audio block and calls `iplPathEffectApply`; it does **not** multiply Direct playback attenuation onto pathing wet (`pathing_wet_uses_playback_distance_attenuation` stays false; PATH-H01 / PR #60 deliberately not restored — that would double-apply 1/d on line-of-sight).

Two Steam distance measures:

- **Line of sight** (`starts = ends = -1`): `evaluate(|source - listener|)` — Euclidean falloff while walking away in open space.
- **Occluded + valid probes**: `evaluate(baked probe-to-probe path length)` via `SoundPath::toVirtualSource` / probe-center distances. Live listener walk offset is **not** in `evaluate()`. Loudness changes when the probe neighborhood changes (new listener probes enter the influence spheres).
- `findPaths == false` (no valid probes): `calcAmbisonicsCoeffsForPaths` does not run; Steam still publishes prior `pathingState` — last SH can freeze. Check probe coverage.

**Bake probe radius:** Steam UniformFloor sets `Probe.influence.radius = spacing` (hard cutoff for `getInfluencingProbes`). Nexus `bake_manual_grid` must do the same (`probe_grid_influence_radius(spacing)`). Older batches that stored `radius = 1 m` with `spacing = 2 m` had non-overlapping spheres, so occluded pathing often stayed on one probe pair until a rebake.

### Accepted residual: Custom scene RunPathing on main thread (Path-03)

With `IPL_SCENETYPE_CUSTOM`, `RunPathing` runs on the main/physics thread inside `tick()` under `simulation_mutex` (see **Custom scene threading** above). `ResonanceRuntime` logs `warn_custom_tracer_main_thread_sim` once. No worker offload unless Godot exposes thread-safe physics queries.

### Accepted residual: deferred reflection mixer soft-cap (F-08)

When `MixerReadGuard` reader wait times out during a mixer swap, releases are deferred instead of forced (`reflection_mixer_release_deferred_`, R-09). A target cap (`kMaxDeferredReflectionMixerReleases` = 16) evicts oldest handles with `iplReflectionMixerRelease` only when `readers == 0`. If readers stay stuck above zero, the queue may grow past the cap (`mixer_deferred_soft_cap_exceeded`); handles are never discarded. **Hardening:** the last guard drop sets `reflection_mixer_deferred_release_pending_` so main/worker drain the queue promptly without calling IPL release from the audio thread. **Why kept:** bounding growth with `readers > 0` would require force-release (UAF) or dropping IPL handles (leak).

### Convolution / TAN wet path (Steam Audio contract)

`iplSourceGetOutputs(REFLECTIONS)` always returns the source-owned TripleBuffer IR handle. `RunReflections` commits new IR data asynchronously; it does not invalidate the handle. Each audio block must call `iplReflectionEffectApply` with that handle. When no new IR was committed, Overlap-Save continues via `mPrevFFTIR`. `ir == nullptr` aborts convolution (no hold). Mix level 0 must still Apply with silence so Overlap-Save drains; skipping Apply on mute then snapping to full wet on unmute clicks. `iplReflectionMixerApply` IFFT-s the mixer spectrum then `reset()`s it — without EffectApply in that block the bus is silent. Nexus therefore feeds the shared mixer every block whenever fetch returns a non-null IR, including on a stale cache epoch. EOS `iplReflectionEffectGetTail` likewise has no cache-epoch gate: Conv/TAN drain while a live mixer handle exists. Continuity is Steam-owned, not Nexus epoch-gating. `iplReflectionEffectCreate.irSize` matches the Apply clamp (`convolution_ir_max_samples` when set). Direct-only sources (reflections + pathing disabled) skip wet IPL allocation on playback prewarm.

## Double-Buffering

- **Parametric / reflection / pathing / occlusion caches**: Worker writes the back slot, bumps epoch, flips `*_cache_front_`. Audio may use stale-epoch reflection params for **parametric-only** hybrid tails and for **convolution/TAN** when `ir != nullptr` (`reflection_stale_epoch_usable_for_mix`; Steam TripleBuffer continuity).
- **ResonancePlayer voices**: `playback_snap_[0|1]` published after register/unregister (see Lock-Free Audio Paths).



## API Limits (resonance_constants.h)

- `kMaxSimulationSources` = 32
- `kMaxProbeBatches` = 1024
- `kMaxProbesPerVolume` = 65536
- `HandleManagerBase::alloc_handle()` returns -1 on overflow (next_handle >= INT32_MAX)



## IPL Handle Cleanup

Use `IPLScopedRelease<T>` from `resonance_ipl_guard.h` for exception-safe release of IPL resources when RAII is preferred over manual cleanup chains.
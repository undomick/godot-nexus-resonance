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

| Node | Base | Role |
|------|------|------|
| `ResonanceRuntime` | `Node` | Native runtime orchestrator (`src/resonance_runtime*.cpp`); drives GDScript helpers; see [ADR-001](adr/001-native-resonance-node-migration.md) |
| `ResonanceListener` | `Node3D` | Listener pose → `ResonanceServer` |
| `ResonancePlayer` | `AudioStreamPlayer3D` | Spatial source + internal stream |
| `ResonanceAmbisonicPlayer` | `AudioStreamPlayer` | HOA bed decode |
| `ResonanceFmodEventEmitter` | `Node3D` | Child of `FmodEventEmitter3D`; FMOD bridge source sync |
| `ResonanceCodaEventEmitter` | `Node3D` | Coda event playback + `ResonanceCodaBridge` spatial link |
| `ResonanceProbeVolume` | `Node3D` | Probes / bake volume |
| `ResonanceStaticScene` | `Node3D` | Static scene asset |
| `ResonanceGeometry` / `ResonanceStaticGeometry` / `ResonanceDynamicGeometry` | `Node3D` | Export / runtime geometry |

Implementation: `src/resonance_fmod_event_emitter.cpp`, `src/resonance_coda_event_emitter.cpp` (ADR-001 Phase 1.1/1.2).

## Initialization Flow

1. `initialize_nexus_resonance_module(MODULE_INITIALIZATION_LEVEL_SCENE)` registers all classes
2. `ResonanceServer` singleton is created and registered
3. `ResonanceSteamAudioContext::init()` creates IPL context, HRTF, Embree/OpenCL/TAN
4. `_init_scene_and_simulator()` creates IPLScene, IPLSimulator, ReflectionMixer
5. Worker thread starts for the simulation tick loop when `scene_type` is not **Custom** (`IPL_SCENETYPE_CUSTOM`). With Custom (Godot Physics), Phonon runs on the main/physics thread inside `ResonanceServer::tick()` and no worker is started.

## Shutdown Order (Critical)

1. `uninitialize_nexus_resonance_module` calls `ResonanceServer::shutdown()`
2. Worker thread joined, `thread_running = false`
3. Steam Audio resources released: mixer → simulator → scene → context
4. Singleton unregistered and deleted

**Do not** call `ResourceSaver.remove_resource_format_saver` / `ResourceLoader.remove_resource_format_loader` in `_exit_tree`; Godot may tear down before plugin exit, causing SIGSEGV.

## Lock Order (Mutex Hierarchy)

IPL scene/simulator work and the worker phonon tick are serialized by `simulation_mutex`. Other mutexes are domain-specific; avoid holding `simulation_mutex` while waiting on `AudioServer::lock`.

When both registry and simulation state are needed:

1. `probe_batch_registry_.mutex_` (load/remove/revalidate probe batches)
2. `simulation_mutex` (scene commit, sources, bake, `_run_phonon_simulation_locked`)

Separate (do not nest with `simulation_mutex` unless a call site documents it): `pending_source_lifecycle_mutex_`, `worker_mutex`, `pathing_vis_mutex`, `_attenuation_callback_mutex`, `dynamic_instanced_transform_queue_mutex_`, `ipl_context_clients_mutex_`.

## Thread Contexts

| Context | Code |
|---------|------|
| Main thread | `update_source`, `load_probe_batch`, `tick`, probe/volume/player updates, `prewarm_steam_audio`, `ResonanceAudioEffectInstance::try_prewarm_processor` |
| Audio thread | `fetch_reverb_params`, `fetch_pathing_params`, `get_source_occlusion_data` (epoch caches only), `ResonanceStreamPlayback::_mix`, `ResonanceAudioEffectInstance::_process` |
| Worker thread | `_worker_thread_func`, `iplSimulatorRunDirect`, `RunReflections`, `RunPathing`, `_worker_sync_fetch_caches` (`iplSourceGetOutputs`) |
| Callbacks | `_pathing_vis_callback`, `_distance_attenuation_callback` run in worker/simulation context |

## Lock-Free Audio Paths

- **Simulation outputs**: Worker fills occlusion/reflection/pathing double buffers under `simulation_mutex`; audio reads published front slots by epoch (no `simulation_mutex` on fetch).
- **Reflection mixer**: `std::atomic<IPLReflectionMixer>` plus `MixerReadGuard` (reader count). Swaps/releases run on main/worker only.
- **Listener pose**: Seqlock on `listener_coords_latest_` / `listener_seq_` (`get_current_listener_coords`).
- **Spatial output gate**: `global_triangle_count` (atomic), `phonon_scene_audio_ready_`, `spatial_audio_warmup_passes_remaining_` (`is_spatial_audio_output_ready`).
- **ResonancePlayer polyphony**: Main mutates `internal_playbacks_` under mutex, then publishes `playback_snap_[2]`; `ResonanceReverbPlayback::_mix` reads the snapshot without locking.
- **ResonanceStreamPlayback source**: `update_parameters` (main) retains `IPLSource` into atomics; `_sync_params` (audio) only copies the handle and reads simulation caches, never `SourceManager::mutex`. On tree exit, `ResonancePlayer` pushes `source_handle = -1` to all playbacks before `destroy_source_handle`.
- **Logging**: `ResonanceLog` posts to a lock-free ring from non-main threads; `ResonanceServer::tick` drains to Godot on the main thread.

## Spatial output startup gate

When triangle geometry is registered (`notify_geometry_changed` with non-zero delta), the server clears `phonon_scene_audio_ready_` and resets `spatial_audio_warmup_passes_remaining_` to `kSpatialAudioWarmupWorkerPasses`. The worker sets `phonon_scene_audio_ready_` after `iplSceneCommit` for that edit. `is_spatial_audio_output_ready()` combines warmup and commit (`spatial_audio_geometry_gate_allows_output` in `resonance_constants.h`); audio and main read it via atomics only.

Until ready, `ResonancePlayer` does not push playback parameters (`params_ever_synced_` stays false), and `ResonanceStreamPlayback::_process_steam_audio_block` zeros spatial output instead of running direct/reflection/pathing. This avoids audible unoccluded output before the Phonon scene matches registered meshes.

## Double-Buffering

- **Parametric / reflection / pathing / occlusion caches**: Worker writes the back slot, bumps epoch, flips `*_cache_front_`. Audio may use stale-epoch reflection params when IR content is still valid.
- **ResonancePlayer voices**: `playback_snap_[0|1]` published after register/unregister (see Lock-Free Audio Paths).

## API Limits (resonance_constants.h)

- `kMaxSimulationSources` = 32
- `kMaxProbeBatches` = 1024
- `kMaxProbesPerVolume` = 65536
- `HandleManagerBase::alloc_handle()` returns -1 on overflow (next_handle >= INT32_MAX)

## IPL Handle Cleanup

Use `IPLScopedRelease<T>` from `resonance_ipl_guard.h` for exception-safe release of IPL resources when RAII is preferred over manual cleanup chains.

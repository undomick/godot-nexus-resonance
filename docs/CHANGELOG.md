# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),  
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **ResonanceRuntime (native)** - C++ node replaces `resonance_runtime.gd`; GDScript bus, activator, perf monitors, and config resource unchanged.
- **ResonanceFmodEventEmitter** / **ResonanceCodaEventEmitter (native)** - Experimental bridge emitters in C++; `resonance_fmod_event_emitter.gd` removed; Coda bridge/sync scripts added.
- **Reflection IR fingerprint** - Baked-energy / IR helpers plus C++ unit tests (`test_reflection_ir_fingerprint*`, `test_reflection_last_good_ir`).
- **resonance_server_worker_sim.cpp** - Simulation worker tick split out of other server translation units.
- **Editor icons** - `resonance_runtime.svg` and `resonance_listener.svg` on nodes; `resonance_config.svg` on **ResonanceRuntimeConfig**, **ResonancePlayerConfig**, and **ResonanceBakeConfig** (`@icon`).
- **Class reference / tooltips** - Reworked docs for **ResonanceRuntime**, **ResonanceListener**, geometry types, **ResonanceProbeVolume**, **ResonanceProbeData**, **ResonanceBakeConfig**, and FMOD/Coda emitters (wiki synced).

### Changed

- **ResonanceServer** - Refactor across lifecycle, fetch, baking, sources, listener, player, probe volume, and scene manager; worker path in dedicated TU.
- **ResonanceProbeVolume** - Docs: `bake_probes()` / `bake_probes_with_floor_points()` deprecated (1.0); inspector **Bake** / `ResonanceBakeRunner.run_bake` is the supported pipeline.
- **Editor bake** - `resonance_editor_job_progress` and `resonance_editor_scene_index` helpers.

## [0.9.18] - 2026-05-07

### Added

- **Runtime baking API** - Added runtime helpers to flush and reload probe volume bakes in memory.
- **Air absorption on baked reverb wet path** - Baked reflection modes (REVERB / STATICSOURCE / STATICLISTENER) now run the wet mono tap through a 3-band air-absorption pre-EQ before `iplReflectionEffectApply`, so distant baked sources lose high-frequency energy the same way realtime ray-traced IRs do (which encode air absorption per ray).
- **Listener-centric probe lookup for baked REVERB** - Baked REVERB now selects the probe nearest the **listener** (instead of the source), so crossing room boundaries no longer plays the wrong IR. Controlled by API `baked_reverb_use_listener_probe` (default **on**) and / or `reflections_sampling_mode` in configs.

### Changed

- `reverb_max_distance` now defaults to **100 m** (was 0 / off). Wet reflections fade out smoothly past this distance instead of staying at full level forever.
- **Wet distance attenuation** - The reverb / pathing wet path now follows a dedicated 1/d falloff (with smooth fade-to-zero in the last 10 % of `min_distance..max_distance`) for **all** attenuation modes — not just `LINEAR` / `CUSTOM_CURVE`. Previously, `INVERSE_NO_SIM` left the wet at unity at any distance and `INVERSE` stayed asymptotic, so distant baked reverb appeared to "stay loud forever". `LINEAR` / `CUSTOM_CURVE` users still see the direct curve win when it is steeper than 1/d. New helper `resonance::reverb_wet_distance_attenuation(dist, min, max)`.

### Fixed

- **Probe bake backups** - Fixed an issue where repeated bakes could create chained `.bak.bak.bak` files and fail to save the latest bake output to the expected resource path.
- **ResonancePlayer.finished** - `finished` now emits when the **dry path** ends (not after wet/pathing tail drain), so equal-length clips fire deterministically regardless of reverb tail length.
- **Wet path distance attenuation in `INVERSE_NO_SIM` / `INVERSE` modes** - `_compute_attenuation` no longer returns 1.0 (constant) for the wet path in `INVERSE_NO_SIM`, and no longer leaves the wet asymptotic in `INVERSE`. Both modes now decay with the new wet 1/d helper and reach zero at `max_distance`.

## [0.9.17] - 2026-05-05

### Added

- **Runtime baking API** - Added `ResonanceRuntimeBaker`to support headless/runtime baking for procedural generation without relying on `EditorInterface` or `ResourceSaver` (bakes into RAM). Contributed by [garthand](https://github.com/garthand)

## [0.9.16] - 2026-05-04

### Changed

- **ResonanceRuntimeConfig** - Removed `simulation_update_interval` (master pacing). Replaced `reflections_sim_update_interval` / `pathing_sim_update_interval` with `reflections_sim_interval` and `pathing_sim_interval` (default 0.1 s each, aligned with **Direct Sim Interval** naming). `ResonanceServer::init_audio_engine` / C++ `ResonanceServerConfig::apply` still read legacy keys `simulation_update_interval` and `*_sim_update_interval` for migration.
- **ResonancePlayerConfig** - Per-source HRTF overrides aligned with `ResonanceRuntimeConfig` naming: `apply_hrtf_to_reflections` → `reverb_binaural_override`, `apply_hrtf_to_pathing` → `pathing_binaural_override`; inspector groups `direct_binaural_override`, `reverb_binaural_override`, `pathing_binaural_override` with `spatial_blend` and `use_ambisonics_encode` under **Spatialization** (bus routing stays under Output). `ResonancePlayer` still reads legacy keys `apply_hrtf_to_reflections` / `apply_hrtf_to_pathing` from older `.tres` until resources are re-saved.

### Fixed

- **ResonancePlayer stream tail (EOS / partial last frame)** - End-of-stream handling no longer stretches a long multi-frame “hold last sample” taper across callbacks (which could flatten the waveform into a DC plateau). Partial-frame EOS uses the same linear ramp as live playback; synthetic output-ring underrun fade is capped; optional short amplitude taper on the last real decoder samples keeps the tail sounding like a decaying wave instead of a slope-to-zero pad.

## [0.9.15] - 2026-05-01

### Added

- **ResonanceAmbisonicPlayer (HOA bed orientation)** - Optional bed orientation via `use_bed_scene_orientation` and `ambisonic_orientation_node` (or first `Node3D` parent): decode orientation is built from combined row-major bed local-to-world and listener world-to-head matrices so the HOA field can follow a scene node while the camera supplies the listener pose. Inspector/class reference tooltips were expanded for the Ambisonic decode group (including `apply_output_gain` and related toggles).
- **Adaptive realtime reflection rays** - When `reflections_adaptive_budget_us` > 0, the worker lowers or restores `numRays` based on measured reflection time vs budget; tune floor/recovery via `reflections_adaptive_ray_min`, `reflections_adaptive_ray_recover_frac`, `reflections_adaptive_ray_recover_cap`, plus interval backoff (`reflections_adaptive_step_sec`, `reflections_adaptive_max_extra_interval`, `reflections_adaptive_decay_per_sec`) on over-budget ticks. Exposed on **ResonanceRuntimeConfig** / server config and covered by config tests.

### Changed

- **ResonancePlayerConfig** - Per-source HRTF overrides aligned with `ResonanceRuntimeConfig` naming: `apply_hrtf_to_reflections` → `reverb_binaural_override`, `apply_hrtf_to_pathing` → `pathing_binaural_override`; inspector groups `**direct_binaural_override`**, `**reverb_binaural_override`**, `**pathing_binaural_override**` with `**spatial_blend**` and `**use_ambisonics_encode**` under **Spatialization** (bus routing stays under Output). `ResonancePlayer` still reads legacy keys `apply_hrtf_to_reflections` / `apply_hrtf_to_pathing` from older `.tres` until resources are re-saved.
- **ResonanceRuntimeConfig** - Removed `simulation_tick_throttle` and `geometry_update_throttle`. They were Nexus-only extras that duplicated Steam Audio's single **Simulation Update Interval** idea (`simulation_update_interval` plus optional `reflections_sim_update_interval`, `pathing_sim_update_interval`, and `dynamic_scene_commit_min_interval`). The worker now wakes every **ResonanceRuntime** tick without frame skipping; moving-acoustic geometry notifies scene commits every transform as before when throttles were set to 1.
- **Lock-free audio-thread paths** - Reverb / reflection / pathing parameter caches use atomic double-buffer fronts and epochs; mixer snapshot swaps and reflection pending state avoid mutex work on the mix thread; cross-thread coordination relies on the worker + atomic flags where previously maps/sets were walked under lock (see migration from mutex-heavy fetch paths in prior iterations).
- **Per-source mix gates Steam Audio simulation flags** - `ResonanceServer` receives `direct_mix_level`, `reflections_mix_level`, and `pathing_mix_level` with each source update. Pathing and reflections simulation are skipped when the corresponding mix is ≤ 0; **direct** simulation is omitted only when **all three** mixes are 0 (if reflections or pathing still need the direct path, e.g. wet occlusion, direct stays enabled). Reduces worker load when sources mute a path via mix alone (Inspector toggles unchanged).

### Fixed

- `ResonancePlayer.finished` now emits reliably (playback no longer gets stuck in a silent "tail" state).
- **“No reverb params” playback warning** - Only logged when `reflections_mix_level` > 0 (pathing-only / reflections-muted setups no longer trigger a misleading “probe baked?” hint). Message names the **ResonancePlayer** node and distinguishes convolution/TAN vs parametric/hybrid guidance.

## [0.9.14] - 2026-04-30

### Fixed

- **Reverb drop/pop at the end of short audio clips** - Mixer return now avoids pulling the shared reflection mixer when no sources have fed it in the current audio quantum (Godot scheduling + mixer mutex ordering edge case vs Unity's deterministic DSP chain). This prevents an audible wet-level dip right as one-shot clips end.

## [0.9.13] - 2026-04-29

### Added

- **AnimationPlayer support** - New `ResonancePlayer.play_animation_audio_clip(stream, from_position)` for AnimationPlayer method tracks. Editor tool to convert existing AnimationPlayer audio clips (**Project → Tools → Nexus Resonance**), plus runtime option `convert_animation_audio_tracks_at_runtime` to apply the same conversion once per scene.
- **Multiple bake entries per probe volume** - `ResonanceProbeVolume.bake_sources` and `bake_listeners` now bake every valid entry (previously only the first). Combined with per-player `reflections_type = Baked Static Source` + `current_baked_source`, this allows position-dependent baked IRs for several fixed emitters in one volume.
- `**ResonanceRuntimeConfig.apply_distance_curve_to_reflections`** - Baked / realtime reverb now fades with the per-source 3D distance curve. Turn off for constant-gain wet feeds (2D ambience beds). Per-source override available.
- `**ResonanceRuntimeConfig.apply_occlusion_to_baked_reflections`** (default **off**, opt-in) - Damps **Baked Reverb** input by the direct-path `occlusion × transmission` factor. Useful for sealed-room where outdoor emitters would otherwise leak a reverb tail indoors. Off by default to keep legitimate around-corner reverb. Per-source override available.
- **Per-source wet-path overrides on `ResonancePlayerConfig`** - `apply_distance_curve_to_reflections_override`, `apply_occlusion_to_baked_reflections_override`, and `reverb_transmission_amount_input` / `reverb_transmission_amount` let individual sources override the global policy (typical setup: enable occlusion damping only on outdoor thunder/rain emitters).
- **Audio-thread health monitors** - `Audio/output_underruns_total` (Core), `Audio/late_mix_total`, `Audio/max_block_time_us`, `Audio/active_voice_count` (Standard). Watch these to diagnose audio-thread starvation versus main-thread / GPU stalls.
- **Server source / probe-batch counters** - `ResonanceServer.get_active_source_count()` / `get_active_probe_batch_count()` plus matching `Server/active_source_count` (Core) and `Server/active_probe_batch_count` (Full) monitors. Lock-free, safe to poll per frame.

### Deprecated

- `**ResonanceProbeVolume.bake_probes()` and `bake_probes_with_floor_points()`** - Reflection-only bake paths. `ResonanceBakeRunner.run_bake([volume])` is a strict superset (pathing + static-source + static-listener passes, automatic re-export of stale `ResonanceStaticScene` assets, undo backup, full incremental-rebake bookkeeping). Calling the native API now emits a deprecation warning. Scheduled for removal in 1.0.

### Changed

- **Main thread no longer blocks on the simulation mutex** - `ResonancePlayer.play()`, dynamic geometry updates, and audio-thread fetches are decoupled from the worker's reflection ticks. Source add / remove / update operations are queued for the next worker tick; transforms flip an atomic; audio-thread fetches always serve the worker-populated double-buffered caches. Eliminates frame spikes when interacting with spatial sources.
- **Steam Audio effects pre-allocated on the main thread** - `ResonanceStreamPlayback` allocates effects and IPL audio buffers when the playback is instantiated instead of inside the first `_mix` call, removing multi-millisecond stalls on `play()` for polyphonic sources.
- **Reflection / convolution mixer feed** - Convolution and TAN scale the input to `iplReflectionEffectApply` with `reflections_mix_level × source linear volume` (no extra `reverb_pathing_attenuation` or air absorption on that tap). When `reverb_max_distance > 0`, an additional linear 1→0 wet factor is applied to convolution / TAN feed, parametric / hybrid wet output, and pathing stereo.
- **Performance monitor tiers re-balanced** - `Worker/sync_fetch_reflections_us` and `Main/last_dynamic_transform_enqueue_us` promoted to **Standard** (correlate with reflection / dynamic-geometry stutter); `Main/runtime_server_tick_us` and `Main/runtime_physics_tick_us` moved to **Full** (only receive values under those tiers anyway). `Worker/last_wake_heavy` renamed to `Worker/heavy_tick_flag` to clarify the 0/1 semantics.
- **Convolution output block-size mismatch handling** - Reverb mixer decode now carries leftover stereo samples across callbacks when `frame_count < audio_frame_size` instead of dropping them, reducing choppy output during temporary frame-size mismatch windows before reinit.

### Fixed

- **Around-corner baked reverb going near-silent** - Baked Reverb occlusion damping is now opt-in (`apply_occlusion_to_baked_reflections`, default off), so a source behind a corner keeps its reverb tail.
- **Baked reverb not fading with distance** - The reflection-effect input now scales with the per-source 3D distance curve. Previously the wet feed used constant gain regardless of distance.
- **Baked reverb / pathing after audio reinit** - Probe batches request an immediate heavy tick on registration, and the runtime reloads probe volumes in the same frame as static geometry. Fixes baked-only stalls where parametric / hybrid `fetch_reverb_params` stayed false and pathing outputs stayed stale until the next simulation interval.
- **Multiple `ResonanceStaticScene` after audio reinit** - After `reinit_audio_engine` (e.g. reflection type or frame size change), every static scene is reloaded additively instead of only the first one.
- **Probe bake vs. multiple static scenes** - The static-scene params hash now combines all `ResonanceStaticScene` assets and transforms in DFS order, so changing or moving any nested static pack marks bakes stale. Existing single-scene bakes may show outdated until re-baked once.
- `**fetch_reverb_params` distance gate** - Honors per-source reflection-distance gating so disabling reflections beyond `realtime_reflection_max_distance` is no longer bypassed via the audio-thread cache path.
- **Parametric/Hybrid split routing** - Each player's effective reverb bus is now passed to `set_reverb_split_output`, so split wet can target a dedicated reverb bus while dry stays on another bus.
- **AnimationPlayer audio clips with ResonancePlayer (Windows)** - Avoids a crash; Steam Audio works via method tracks or via the new conversion tool.
- `**ResonanceStreamPlayback` teardown / mix safety** - `_stop` halts the nested base playback before unregistering from the player to reduce teardown races; `_mix` clamps `mix_audio()` sample count to the requested frame count.
- **Reverb / pathing tail abruptly cut when a one-shot stream finished or `stop()` was called** - Per-source playbacks now stay alive long enough for the parametric / convolution / hybrid reverb tail and the pathing tail to decay naturally. `ResonancePlayer.stop()` performs a soft stop (halts the dry input but keeps the playback mixing while the IPL effect tails and ring buffers drain), and `_mix` always returns the requested frame count so the AudioServer keeps polling until the tail is exhausted (capped by `max_reverb_duration` as a safety net). `ResonancePlayer.stop()` is now also exposed to GDScript.
- **Audio-thread debug I/O removed from runtime path** - Temporary NDJSON file logging hooks were removed from listener/path processors to prevent filesystem/mutex stalls in the realtime mix path.
- **Ambisonic stop now drains pending output** - `ResonanceAmbisonicInternalPlayback` keeps mixing until its internal input/output rings are empty after `stop()`, reducing abrupt tail truncation on stop events.
- **Reverb output sanitization before final clamp** - Reverb bus now sanitizes NaN/Inf samples before output limiting to avoid propagating invalid floats under edge cases.
- `**Export Active Scene to OBJ` - "Can't find file ... during file reimport" error** - The newly written OBJ is now registered with `EditorFileSystem.update_file()` before `reimport_files()`, so Godot can locate and import it immediately instead of failing because its filesystem cache was stale.
- **Probe bake silently used stale `ResonanceStaticScene` asset** - Adding new `ResonanceGeometry` after the last static-scene export caused the bake to ignore the new geometry. The bake runner now compares `ResonanceStaticScene.export_hash` to the live scene hash and re-exports the static asset automatically before baking when they diverge.
- **Mojibake in bake / probe-batch logs** - Replaced U+2014 em-dashes with ASCII `-` in the `UniformFloor returned 0 probes` fallback message and the `reload_probe_batch skipped` warnings so they render correctly in CP-1252 / non-UTF-8 terminals.
- `**ResonanceRuntimeConfig` change signals were dead in play mode** - `_connect_runtime_signals` returned immediately outside the editor, so `reflection_type_changed`, `pathing_enabled_changed`, and `audio_frame_size_changed` never fired during gameplay. Settings menus that tweaked these at runtime had no effect until scene reload. Connect path now mirrors `_disconnect_runtime_signals` and runs in both editor and play mode.
- `**ResonanceProbeVolume.set_probe_data` ignored hot-swaps at runtime** - Assigning a different `ResonanceProbeData` to a live volume kept the previously registered IPL probe batch handle, so reverb / pathing / hybrid stayed on the old probes. The setter now calls `reload_probe_batch()` automatically when the data Ref actually changes during play.
- `**ResonanceStaticScene.set_static_scene_asset` did not rebuild the IPL scene** - Swapping a static-scene asset at runtime (level streaming, dynamic loaders) left Steam Audio on the previous merged static mesh until the next `reinit_audio_engine`. The setter now requests a deferred, debounced `request_static_scene_reload()` on `ResonanceRuntime`.
- `**ResonanceGeometry.set_material` did not refresh IPL meshes** - Swapping a `ResonanceMaterial` Ref kept Steam Audio on the previous absorption / scattering / transmission coefficients. The setter now triggers `_create_meshes()` when the geometry is in the tree.
- **Inspector "Probes baked" status lied for multi-source / multi-listener volumes** - `get_volume_bake_status` only inspected the first NodePath in `bake_sources` / `bake_listeners`, so moving any later entry left the volume reported as up-to-date. Status now mirrors the bake pipeline's `compute_position_radius_list_hash` for multi-entry lists.
- **Native `ResonanceProbeVolume.bake_probes()` produced inconsistent metadata** - Pathing / static-source / static-listener / static-scene hashes are populated by `ResonanceBakeRunner.run_bake()` only. The native API is now deprecated (see *Deprecated* above); calls emit a warning that points users at `run_bake()`. Probe data with a missing `bake_params_hash` is now treated as dirty (was: silently trusted as "valid legacy data") so a forced re-bake catches pre-hash leftovers.
- `**Export Active Scene` returned a stale ResourceLoader cache on repeat exports** - `load(save_path)` after `ResourceSaver.save` could hand back the previously cached `ResonanceGeometryAsset` if the same path had been loaded earlier in the session. Switched to `ResourceLoader.load(..., CACHE_MODE_REPLACE)`.

### Documentation

- `**wiki/ResonanceProbeVolume-and-BakeConfig.md`** - Documented native bake limitations, post-bake world-fixed probe positions, and the intentional `bake_num_threads` exclusion from the params hash.
- `**wiki/ResonancePlayer-and-PlayerConfig.md`** - Documented the ~15-frame override propagation latency and how to force an immediate switch.

## [0.9.12] - 2026-04-13

### Added

- **Export: text or binary** - In **Project Settings → Nexus → Resonance → Export** you can save exported geometry and probe bake data as usual text (`.tres`) or as smaller binary (`.res`) files. After changing the option, export or bake again. New probe files use a `*_batch` name pattern; old `*_baked_probes*` files are still found when cleaning up.
- **Custom physics + many reflection rays** - If reflections use Godot's physics, you can choose how many rays are handled in one batch (default 16) for smoother CPU use; set to **1** for the old behavior. Does not affect the other scene types.
- **More runtime knobs** - Reflections and pathing can use separate update rates if you need that. Realtime reflections can ignore sources beyond a set distance from the listener (off by default).
- **Profiling** - Optional timing for the convolution audio path and matching entries under **Debugger → Monitors**.

### Changed

- **AmbisonicsPlayer** - Per-channel stream slots were replaced by a clear **order** in the inspector (1st / 2nd / 3rd) and a matching channel list. The player groups decode options and warns if the stream type does not fit.
- **Realtime rays** - The zero-rays option is labeled **Off**; quick presets include 8 / 16 / 32 rays.
- **Steam Audio debug** - Validation and CPU-related engine checks moved from the shared config resource to the **ResonanceRuntime** node (**Runtime Debug**). Set them there again if they lived on a saved `.tres`.
- **Baked-only reverb** - Less unnecessary work when only baked probe data drives the reverb.
- **Performance monitors** - **Off** / **Core** / **Standard** (default, fewer graphs) / **Full** (everything as before).

### Fixed

- **Reflections + convolution** - Fixed a slowdown that made hybrid/convolution setups much heavier than intended.
- **Closing the game** - Rare crash when quitting a built game should be gone.

## [0.9.11] - 2026-04-04

### Fixed

- **Custom scene (Godot Physics)** - If 3D physics runs on its **own thread** (for example with Jolt), live occlusion and reflection rays no longer trigger endless *space not accessible* errors. Audio simulation now lines up with the physics step; debug ray lines follow the same timing. The performance overlay can show how long that physics-aligned tick takes.

### Changed

- **Editor & addon scripts** - Baking, export/cleanup paths, and runtime wiring were split into smaller internal modules; **how you use the inspector and menus is unchanged**. Probe cleanup is a bit smarter about which baked files are still referenced.
- **HRTF SOFA asset** - Normalization in the inspector uses clearer enum names (**None** / **RMS**).

## [0.9.10] - 2026-04-02

### Added

- **Custom scene type (3)** in **ResonanceRuntimeConfig** - live occlusion and reflection rays follow **Godot physics** (your colliders), not the exported acoustic mesh. Optional `**resonance_physics_material_preset`** on colliders; **ResonancePlayer** can auto-ignore its own collision bodies. **Baking** still uses the usual Embree/Default path.

### Fixed

- **Debug overlay (ResonancePlayer)** - **Signal P** now shows **path wet output RMS** (per block, clamped 0..1), aligned with **Signal D/R** as pipeline-derived levels.
- **ResonancePlayer** - Sources that started before **ResonanceServer** was ready (common when the player is a **child** of **ResonanceRuntime**) now **register correctly**; reverb/occlusion should no longer stay permanently “dry” from that alone. `**play()`** and `**play_stream()`** both hook up the same way.
- **ResonancePlayer polyphony** - With `**max_polyphony` > 1** and `**player_config`**, every voice gets the same Steam parameters (one shared sim source per node); reverb split mixes convolution/parametric wet from all voices. `**get_audio_instrumentation`** aggregates counters and adds `**polyphony_voice_count**`.
- **ResonancePlayer / AudioStreamPlayer3D** - `**stream`** / `**get_stream`** keep the user’s resource while the engine uses an internal wrapper; runtime stream changes update the wrapper. `**max_db`** is honored with volume in native `**_mix`**. Docs: inspector matrix and `**godot_*`** keys in `**get_audio_instrumentation**`.
- **Occlusion** - If the engine briefly has no fresh occlusion data, direct sound defaults to **not** treated as fully blocked (avoids accidental silence).

## [0.9.9] - 2026-03-28

### Added

- **Richer reflection bakes** - Choose **ambisonics order** (1-3) on each probe volume’s **ResonanceBakeConfig**; change it and you’ll be prompted to re-bake when hashes no longer match.
- **Surround speakers for direct sound** - Pick a **speaker layout** (mono through 7.1) in **ResonanceRuntimeConfig**. Without HRTF, surround can use ambisonics-based panning; with HRTF, binaural output is folded to stereo for the main player path. Changing layout needs **reinit audio engine** like other native audio settings.
- **Probe volume maintenance** - Inspector section **Probe batch (advanced)**: remove a single probe by index, or drop baked **pathing** / **reflections (reverb)** layers when you want to trim data without a full probe re-bake (you may still need to bake again afterward).

### Fixed

- **Serialized Phonon scene I/O** - `**save_scene_data`** / `**load_scene_data`** resolve `**res://`** and `**user://`** through `**ProjectSettings.globalize_path`** before `**FileAccess`** open/exists checks, matching other export paths and avoiding platform-dependent relative-path surprises.
- **Debug OBJ export staging** - Failed or incomplete atomic OBJ writes now remove `**_nexus_obj_staging`** contents (and the folder when empty) instead of leaving stale `.obj`/`.mtl` fragments; successful exports drop the empty staging directory as well.
- **ResonanceLogger console** - Each log line is mirrored with `**print()`** in addition to `**print_rich()`** so messages show under standard Output filters; `**_ready()`** reloads logger project settings (and file output flags now load even if `categories_enabled` was missing). New Project Setting `**nexus/resonance/logger/output_to_debug`** (default on) toggles console mirroring.
- **Steam Audio verbose** - Project setting is read with proper bool coercion; IPL INFO/DEBUG forwarding unchanged but **one summary line** is printed at context init when enabled so the option is visibly active. **Editor:** `steam_audio_verbose` is always re-registered in Project Settings (not only on first creation).
- **Short sounds** - Parametric/hybrid reverb and pathing tails decay naturally after the dry signal ends, so one-shots can sound like they’re in the room instead of cutting off abruptly.
- **ResonanceRuntime** (**breaking**) - The runtime node is a plain **Node** again (fixes a regression). Update scenes if you depended on the previous setup.

## [0.9.8] - 2026-03-26

### Added

- **ResonanceRuntimeConfig** - HRTF volume (dB) and normalization (None/RMS) for the embedded default HRTF; **SOFA list** (`hrtf_sofa_assets`) plus `hrtf_sofa_selected_index`; **max occlusion samples** and **max simulation sources** caps; **context validation** toggle exposed in the inspector and passed through `get_config()` to the native engine.
- **Smoother occlusion & transmission** - `ResonancePlayerConfig.playback_coeff_smoothing_time` (seconds; `0` = off) reduces harsh jumps at geometry boundaries.
- **Export** - Nested `ResonanceStaticScene` with its own asset is no longer merged again into the parent static export (fewer duplicates for instanced sub-scenes). Baked probe `.tres` paths include scene location so identical volume names under different branches collide less often.
- **Debug & monitors** - Custom **Nexus Resonance** performance monitors in **Debugger → Monitors**  
`ResonanceServer.get_simulation_worker_timing()` and reverb-bus expert counters expose the same worker breakdown.

### Changed

- **ResonanceRuntimeConfig** - Removed redundant `hrtf_sofa_asset`; use `hrtf_sofa_assets` only (e.g. one element for a single SOFA file).
- **Pathing wet level** - Aligns with Steam Audio Unity/FMOD spatialize: no extra multiply by `reverb_pathing_attenuation` (distance is already in baked path SH). `pathing_mix_level` ramps the **mono input** before `iplPathEffectApply`, not the stereo output.
- **Reflections & pathing level** - Mix ramps within each audio block; overall level and fade-in behavior may differ slightly from 0.9.7.
- **Defaults** - Simulation CPU budget default **15%** (was 5%). Runtime **path validation** defaults **on** and **find alternate paths** **off** on `ResonanceRuntimeConfig`; per-source **Use Global | Disabled | Enabled** via `path_validation_override` / `find_alternate_paths_override` on `ResonancePlayerConfig` (replaces bool player fields and removes `pathing_validation_ab_mode`). `pathing_binaural_override` defaults to **Use Global** (formerly `apply_hrtf_to_pathing`). `max_transmission_surfaces` default **16**. Distance attenuation: "**Disabled" added**.

### Fixed

- **Editor - ResonanceRuntime Doc-Button was opening the Documentation of "Node"**. This is sadly a breaking change. Use **Change Type** or recreate the node.
- **Debug OBJ export** - No more duplicate reimport tasks / broken progress; files are written atomically.
- **Geometry vs. export** - Runtime static/dynamic acoustic meshes use the same world rules as export (`geometry_override` vs parent `MeshInstance3D`), fixing wrong placement or scale (with assets imported with Nexus Importer).
- **ResonancePlayer** - Volume db is now functional. Multiplies signals from Steam Audio.

## [0.9.7] - 2026-03-24

### Fixed

- **Crash when loading another scene or reloading audio settings while sound is playing** - especially reported on Linux after changing ray-tracer options (e.g. Embree) and switching levels: the game could quit with a native crash. You can change scenes and let the engine restart audio without losing stability; spatialized playback and convolution reverb recover cleanly instead of taking the process down.

## [0.9.6] - 2026-03-23

### Added

- **Editor export plugin** - Resonance now registers for **Linux**, **macOS**, and **Android** (in addition to Windows) and calls `add_shared_object` for the same native libraries as `[dependencies]` in `nexus_resonance.gdextension` (`libphonon.so` / `libphonon.dylib` per platform; per enabled Android ABI under `bin/android/<abi>/`). Warns when Android **armeabi-v7a** or **x86** is enabled but no matching `libphonon.so` is shipped in the addon.
- **ResonanceRuntime** - **enable_debug**: when `false`, debug/performance/player overlay toggle keys are ignored; turning it off at runtime closes overlays and player ray viz.

### Fixed

- **Sound at level start (ResonancePlayer)** - With Autoplay or other audio that starts as soon as the scene runs, you no longer get a brief flash of „unblocked“ direct sound before the world feels ready. Playback stays quiet until spatial audio can respect your geometry and occlusion, so the first thing you hear matches what you see (walls and materials in the way).
- **ResonanceDynamicGeometry on phones and tablets** - Moving or animated acoustic geometry (doors, platforms, props) is now taken into account on Android builds the same way as on desktop, so sound can be blocked or reflected by those objects instead of behaving as if they were not there.

### Changed

- **ResonanceRuntimeConfig** - **Breaking Change:** default **Scene Type** is now **Default (0, built-in Phonon tracer)** instead of Embree. 
- **ResonanceRuntime** - Replaced **debug_sources** with **player_overlay_toggle_key** (default F3) to toggle source/occlusion + reflection ray visualization like the other overlays.
- **ResonanceRuntime** - Default overlay keys: debug **F1**, performance **F2**, player **F3** (when **enable_debug** is on).
- **Project Settings (Nexus → Resonance)** - **Bus** / **Reverb Bus Name** removed (configure buses on **ResonanceRuntimeConfig**). 
- **Debug overlay** - Overhauled.

### Removed

- **ResonanceRuntime** - `performance_overlay_enabled` (performance overlay only via **performance_overlay_toggle_key**).
- **ResonanceRuntime** - `debug_sources` (use **player_overlay_toggle_key** instead).

## [0.9.5] - 2026-03-21

### Changed

- **Project Settings layout** - Resonance options moved from **Audio → Nexus Resonance** (`audio/nexus_resonance/`*) to **Nexus → Resonance** (`nexus/resonance/`*). Opening the project with the addon enabled migrates legacy keys and removes the old paths.

### Fixed

- **ResonanceDynamicGeometry + scene change** - Phonon teardown order for dynamic objects (global `InstancedMesh` off first, `iplSceneCommit`, sub-scene static meshes abandoned then `iplSceneRelease(sub_scene)`; no per-mesh Remove/Release on that Embree sub-scene after detach). Fixes native crash/hang on `change_scene` with moving geometry (e.g. door; MovementTestbed).
- **Geometry notify deadlock** - `notify_geometry_changed_assume_locked` when `simulation_mutex` is already held (`_clear_meshes_impl`, `discard_meshes_before_scene_release`); avoids hang for meshes with `triangle_count > 0`.

### Removed

- Misleading startup warning that implied Convolution/Hybrid/TAN with Realtime Rays = Baked Only (0) required baked probes.

## [0.9.4] - 2026-03-20

### Changed

- **ResonanceServer build layout** - Implementation split from monolithic `resonance_server.cpp` into focused translation units (`resonance_server_lifecycle.cpp`, `resonance_server_callbacks.cpp`, `resonance_server_listener.cpp`, `resonance_server_sources.cpp`, `resonance_server_fetch.cpp`, `resonance_server_scene_io.cpp`, `resonance_server_baking.cpp`, `resonance_server_debug_bind.cpp`). `SourceManager` / `ProbeBatchManager` moved to `handle_manager.cpp`. Public API and `resonance_server.h` unchanged.
- **Android: realtime rays** - `ResonanceRuntimeConfig.get_effective_realtime_rays` no longer forces `0` on Android. Realtime simulation uses the configured `scene_type`; Embree/Radeon Rays fall back to Steam Audio’s built-in (Default) tracer on device.

### Fixed

- **Processor init / ambisonic input bounds** - `ResonanceAmbisonicProcessor::initialize` and `ResonanceReflectionProcessor::initialize` reject null `IPLContext`; `ResonanceAmbisonicProcessor::process` avoids out-of-bounds read when interleaved input is shorter than `frame_size * (order+1)^2` (clears stereo output instead of calling `iplAudioBufferDeinterleave` on undersized data).
- **Ring buffer zero capacity** - `RingBuffer::write`/`read` return immediately when `capacity == 0`, avoiding undefined behavior from `% 0` and `capacity - pos` underflow.
- **Bake progress thread safety** - `bake_progress` is emitted on the **Godot main thread** via `call_deferred` (Steam Audio `IPLProgressCallback` may run on worker threads).
- **Reverb after reinit** - Convolution reverb bus (`ResonanceAudioEffect`) resets its `MixerProcessor` when the server’s Steam Audio generation changes, avoiding stale Phonon handles after `reinit_audio_engine`.
- **Empty mesh / bake asset guards** - Dynamic geometry path rejects empty serialized mesh data; bake static-scene path requires non-zero asset size before passing buffers to Phonon.

## [0.9.3] - 2026-03-17

### Added

- **Steam Audio verbose logging** - Project Setting `nexus/resonance/logger/steam_audio_verbose` forwards IPL_LOGLEVEL_INFO and IPL_LOGLEVEL_DEBUG to Godot output when enabled (for debugging).
- **Unit test for pathing_params_hash** - `test_pathing_hash_bake_runner_matches_cpp_format` ensures GDScript and C++ use identical dict format for hash consistency.

### Changed

- **TAN/OpenCL init** - Documented that SEH crash handling is Windows/MSVC-only; TAN init considered Windows-only stable on other platforms.
- **ProbeData loader** - Class doc now documents size limits (data: 256 MiB, probe_positions: 1 MiB) and str_to_var memory considerations.
- **ProbeData saver** - Class doc now documents concurrency: avoid simultaneous saves on the same .tres path to prevent race conditions.
- **iplProbeBatchRetain/Release** - API comments in ResonanceServer and ResonanceProbeBatchRegistry explicitly state that every Retain return value must be released by the caller.
- **resonance_config_constants** - Added `REFLECTION_DISPLAY_NAMES` for debug overlay; documented that BakeConfig supports 0/1/2 only (no TAN).
- **debug_overlay** - Uses `Constants.REFLECTION_DISPLAY_NAMES` instead of local array; TAN (Index 3) now shown correctly; DRY for reverb bus section.
- **performance_overlay** - Connects to `get_viewport().size_changed` so overlay position updates on window resize.
- **resonance_fmod_event_emitter** - Consolidated Limitation docs to single block; `_is_fmod_emitter` uses exact class check (`FmodEventEmitter3D`) instead of substring.
- **resonance_logger** - Comment documenting READ_WRITE concurrency and recommended single-writer usage when multiple processes access log file.
- **resonance_ui_strings** - Documented `DOC_BASE_URL` and anchors.
- **resonance_runtime** - Comment clarifying `load_static_scene_from_asset` as legacy single-scene API.
- **resonance_scene_utils** - `export_type` parameter as `StringName` for consistency.

## [0.9.2] - 2026-03-16

**Stability improvements**

### Fixed

- **ProbeData serialization** - `static_scene_params_hash` is now saved and loaded by the custom ProbeData Saver/Loader. Previously it was lost on save, causing unnecessary re-bakes and wrong "outdated" status.
- **iplAudioBufferAllocate** - Return value checked in `ResonancePlayer` and `ResonanceAmbisonicPlayer`; on failure, cleanup and log instead of using uninitialized buffers.
- **Scene save/load ctx** - Null checks for `ctx` in `save_scene_data` and `load_scene_data` (ResonanceSceneManager) to avoid crashes when server is shutting down.
- **Empty probe data** - ResonanceProbeBatchRegistry rejects empty `PackedByteArray` before `iplSerializedObjectCreate`.
- **MixerProcessor init** - ResonanceAudioEffect now checks `processor.is_ready()` after init; reverb stays silent until init succeeds instead of running with invalid state.

### Changed

- **OpenCL / TAN / RadeonRays** - Error status codes logged on init failure for easier debugging (ResonanceSteamAudioContext).
- **Bake pipeline** - Defensive `Engine.has_singleton("ResonanceServer")` and `srv` null check at start of `_run_bake_pipeline_main_thread`; aborts cleanly if GDExtension is unloaded during bake (e.g. editor close).

## [0.9.1] - 2026-03-15

**Stability improvements**

### Fixed

- **Memory leak on runtime reinit** - `discard_meshes_before_scene_release` now correctly calls `iplStaticMeshRemove`/`iplStaticMeshRelease` and `iplInstancedMeshRemove`/`iplInstancedMeshRelease` before clearing handles. Previously, IPL resources were leaked when changing reflection type or pathing (triggering `reinit_audio_engine`). Phonon uses refcounting; meshes must be explicitly released per Steam Audio API.

### Changed

- **Defensive null checks** - `ResonanceProbeBatchRegistry`: guards for `sim_mutex` before `std::lock_guard` to avoid crash when null. `parse_mesh_to_ipl`: early return when `mesh.is_null()`. `Engine::get_singleton()`: null checks in `ResonanceGeometry` and `ResonanceProbeVolume` before `is_editor_hint()`.
- ** region_size validation** - `ResonanceProbeVolume.set_region_size` clamps each component to minimum `kProbeRegionSizeMin` (0.1) to prevent degenerate volumes.
- **Empty asset guard** - `add_static_scene_from_asset` rejects assets with `get_size() == 0` before passing to Phonon.

## [0.9.0] - 2026-03-14

### Added

- **Performance Overlay** - Optional overlay (FPS, frame time, physics time). Enable via `performance_overlay_enabled` on ResonanceRuntime; toggle with F4. Independent from debug overlay.

### Removed

- **debug_overlay_visible** - Debug overlay now only toggles with F3; no export for initial visibility.
- **debug_reflections** (ResonanceRuntimeConfig) - Merged into `debug_sources` on ResonanceRuntime; one switch controls both occlusion and reflection ray viz.
- **debug_pathing** (ResonanceRuntimeConfig) - Removed.
- **context_validation** (ResonanceRuntimeConfig) - Removed.

## [0.8.8] - 2026-03-10

**Stability and Consistency Update**

### Added

- **InitFlags for ResonanceDirectProcessor** - Bitwise `DirectInitFlags` (DIRECT_EFFECT, BINAURAL_EFFECT, PANNING_EFFECT, BUFFERS, AMBISONICS_ENCODE). `process()` only runs when all required flags are set; avoids partial-init crashes.
- **InitFlags for ResonanceAmbisonicProcessor** - Bitwise `AmbisonicInitFlags` (ROTATION, DECODE, BUFFERS). Aligns with Mixer/Reflection/Path processor consistency.
- **Input-start detection** - `ResonanceInternalPlayback` delays processing until first non-zero input sample. Avoids ramp artifacts when Godot sends incorrect params before playback actually starts.
- **Process-entry guards** - Centralised null checks at start of `_process_steam_audio_block`: context, srv, buffers (sa_in_buffer, sa_direct_out_buffer, sa_path_out_buffer, sa_final_mix_buffer). Early return on invalid state.
- **Passthrough fallback on init failure** - When Direct or Ambisonic processor init fails, pass input through instead of silence. Direct: copy in_buffer to out_buffer. Ambisonic: decode W (omnidirectional) channel to stereo (1/√2). Reflection/Path add to mix; when they fail, dry from Direct is unchanged.
- **HRTF and ReflectionMixer double-buffer** - main/init writes to buffer [1], audio thread reads from [0] via `get_hrtf()` and `get_reflection_mixer_handle()`; atomic flags trigger swap on consume. Prepares for future SOFA hot-reload or config changes.
- **Processor pattern documentation** - DEVELOPERS.md: init order, release order, InitFlags usage, process guards, double-buffering. Note on Steam Audio mix_return_effect buffer bug to avoid when writing lazy-init code.
- **Debug overlay toggle** - `debug_overlay_toggle_key` (default F3) toggles the debug overlay at runtime. When overlay is on: cursor visible, camera ignores mouse input. Restores previous mouse mode when overlay is closed.
- **Reverb Bus Crackling Debug** - Debug overlay section shows `fetch_reverb` lock_ok, cache_hit, cache_miss to diagnose audio dropouts and crackling.

### Fixed

- **Crackling with Ambisonic Order 2/3 and Convolution/Hybrid** - Audio thread no longer blocks on `simulation_mutex`. Non-blocking `try_lock` plus last-known-good cache (like Parametric) for Convolution/Hybrid/TAN. When the worker holds the lock, the audio thread uses cached reflection params instead of blocking → avoids Xruns and crackling.
- **Pathing crackling** - `fetch_pathing_params` now uses `try_lock` plus per-source pathing cache (`CachedPathingParams` with eqCoeffs and SH coefficient copy). When the worker holds `simulation_mutex` during RunPathing, the audio thread uses cached pathing params instead of blocking. Cache invalidated on source/batch removal and shutdown.
- **Reflections despite occlusion** - Transmission is no longer applied to reverb. Reflections are indirect paths that go around obstacles; only the direct path uses transmission. Fixes missing reflections when the listener has no line of sight to the source.

### Changed

- **Double-buffer for audio thread** - Listener coordinates and parametric reverb cache use lock-free double-buffering. Main/worker write to buffer [1], audio reads from [0]; atomic flags trigger swap on consume. Reduces lock contention in the audio hot path (`get_current_listener_coords`, `fetch_reverb_params` parametric fallback).
- **Reflection cache** - Convolution/Hybrid/TAN reflection params are cached per-source when `fetch_reverb_params` succeeds. Cache invalidated on source/batch removal and shutdown.
- **Pathing cache** - Pathing params (eqCoeffs, sh_coeffs) cached per-source when `fetch_pathing_params` succeeds. Double-buffer swap on consume. Invalidated on source removal, probe batch removal, revalidate, clear batches, and shutdown.
- **kMaxBlocksPerMixCall** - Increased from 2 to 4 to better handle Godot frames=1024 with frame_size=512 (more blocks per mix callback).
- **GDExtension unload safety** - `clear_probe_batches` in plugin `_disable_plugin` only runs when `Engine.has_singleton("ResonanceServer")` is true. Avoids crash when editor closes and GDExtension unloads before the plugin.
- **Probe Volume deletion robustness** - ResonancePlayer auto-clears `pathing_probe_volume` when target node is gone; ResonanceProbeVolume `_clear_player_refs_to_this` falls back to tree root when edited scene root is null (e.g. editor teardown).

### Documentation

- **simulation_update_interval** - ResonanceRuntimeConfig: Order 2/3 + Convolution/Hybrid: recommend ≥ 0.2 s if crackling persists.
- **Audio-Buffer-and-Latency** - Added `simulation_update_interval` tip for crackling workaround.

## [0.8.7] - 2026-03-07

**Reliability Update**

### Added

- **Pre-bake audio_data writability check** - Validation checklist now includes "audio_data/ writable" to fail fast when probe data cannot be saved.
- **Unit test for invalid probe data** - Edge case test for malformed data field in ResonanceProbeDataLoader.
- **ResonanceUtils::safe_unit_vector** - Defensive vector normalization with minimum length guard (1e-3) and fallback to avoid NaN/division-by-zero from degenerate transforms.

### Fixed

- **IPL error handling** - All `ipl*Create` calls (iplSceneCreate, iplSimulatorCreate, iplReflectionMixerCreate, iplSerializedObjectCreate, iplProbeBatchCreate) now check `IPL_STATUS_SUCCESS`, log via ResonanceLog, and perform cleanup on failure. Prevents crashes when Steam Audio init or bake fails.
- **Atomic probe data saves** - ResonanceProbeDataSaver writes to `.tmp` then renames atomically to avoid corrupted files on crash or power loss.
- **DirAccess.make_dir failure handling** - Export and bake now check mkdir result and show error when audio_data cannot be created.
- **Defensive vector normalization** - `update_listener` and `_update_source_internal` use `safe_unit_vector` for dir/up/right orthonormalization; prevents degenerated coordinate spaces when inputs have near-zero length.
- **Shutdown atomic flags reset** - `_shutdown_steam_audio` resets pending_listener_valid, simulation_requested, reflection/pathing heavy request flags, scene_dirty, pathing_ran_this_tick at start to avoid late accesses during/after shutdown.

### Changed

- **ResonanceServer init** - `_init_scene_and_simulator` returns bool; on IPL failure cleans up and resets context so is_initialized() stays false.
- **Probe data loader** - Length limits on data/probe_positions expressions to avoid excessive memory use from malformed .tres files.
- **Config clamping** - `ambisonic_order` clamped to 1-3, `max_reverb_duration` to 0.1-10.0 s in ResonanceServerConfig; `max_reverb_duration` export_range in ResonanceRuntimeConfig.
- **Processor InitFlags** - ResonanceMixerProcessor, ResonanceReflectionProcessor, ResonancePathProcessor use bitwise InitFlags; process() guards ensure only fully initialized combinations run (avoids partial init crashes).

## [0.8.6] - 2026-03-04

### Added

- **Ray tracer selection** - `ResonanceRuntimeConfig.scene_type`: Default (0) = built-in Phonon; Embree (1) = Intel, faster CPU; Radeon Rays (2) = GPU. Developers can explicitly choose Default for maximum compatibility or Embree for better CPU performance.
- **Listener notification API** - `ResonanceServer.notify_listener_changed()` and `notify_listener_changed_to(node)` for Splitscreen/VR or manual listener switching. `notify_listener_changed_to` extracts position/direction from a `Node3D` and updates the audio listener.

### Changed

- **Replaced use_radeon_rays with scene_type** - Boolean removed in favor of explicit enum. Backwards compatible: configs with `use_radeon_rays` (no `scene_type`) still work (true→Radeon Rays, false→Embree).
- **Editor menu naming** - "Export Static Scene" → "Export Active Scene"; "Export Dynamic Meshes" → "Export Dynamic Objects In Active Scene"; "Bake All Probe Volumes" → "Bake All Probe Volumes In Active Scene".  
Exports static `ResonanceGeometry` to OBJ+MTL via `iplSceneSaveOBJ` for visualization in external tools. `ResonanceServer.export_static_scene_to_obj(scene_root, file_base_name)` available at runtime and in editor.  
New: "Export Dynamic Objects In All Scenes In Build".

## [0.8.5] - 2026-03-02

**Pro-Source Controls and User-Defined Inputs**

### Added

- **Pro-source reflections type** - `ResonancePlayerConfig.reflections_type`: Use Global, Realtime, Baked Reverb, Baked Static Source, Baked Static Listener. Per-source choice between realtime raytracing and baked data.
- **Current Baked Source/Listener** - `current_baked_source` and `current_baked_listener` NodePaths for runtime-switchable baked references (e.g. teleport to different baked listener zones).
- **User-defined occlusion** - `occlusion_input` (Simulation / User Defined), `occlusion_value` (0-1). Script-controlled occlusion for custom models.
- **User-defined transmission** - `transmission_input`, `transmission_low`, `transmission_mid`, `transmission_high` for script-controlled transmission.
- **User-defined directivity** - `directivity_input`, `directivity_value` (0-1) for script-controlled directivity.
- **Pro-source HRTF** - Reflection and pathing HRTF toggles per source (Use Global / Disabled / Enabled; now `reverb_binaural_override` / `pathing_binaural_override`).
- **Pro-source reflections/pathing toggles** - `reflections_enabled` and `pathing_enabled_override` to enable/disable reflections or pathing per source.

### Changed

- Reflection simulation and baked data variation are now per-source; server `update_source` supports `baked_data_variation` -1 (Realtime), 0 (Reverb), 1 (Static Source), 2 (Static Listener).
- Per-source `reflections_enabled_override` and `pathing_enabled_override` passed to server; sources can exclude reflections or pathing independently.

## [0.8.2] - 2025-02-28

**Audio Bus Flexibility**

### Added

- **Configurable reverb bus** - `ResonanceRuntimeConfig.reverb_bus_name` for flexible routing. Reverb output is sent to the same bus as Direct+Pathing (`bus`).
- **Per-source reverb bus override** - `ResonancePlayerConfig.reverb_bus_override` (Use Global / Custom) and `reverb_bus_name` for selecting the reverb bus per source. Use Global = RuntimeConfig; Custom = pick from existing buses.
- **Project Settings** - `nexus/resonance/reverb_bus_name` for editor/default bus setup.

### Changed

- **Removed reverb_bus_send** - Reverb output now always goes to `bus` (same as Direct+Pathing). Simpler, consistent with player_config which has no separate send.
- Reverb bus name is no longer hardcoded; default remains "ResonanceReverb" and sends to `bus`.
- Direct and pathing sound routing via `ResonancePlayer.bus` explicitly documented (was already supported).

## [0.8.1] - 2025-02-27

**Feature, Performance and Stability Update.**

### Added

- **Sample rate override** - `ResonanceRuntimeConfig.sample_rate_override` enum: Use Godot Mix Rate (default), 22050 Hz, 44100 Hz, 48000 Hz, 96000 Hz, 192000 Hz. Mismatch with Godot mix rate may affect audio (no resampling).
- **Audio frame size 2048** - New option for lower CPU usage at higher latency (Ray Tracer Settings).
- **DEVELOPERS.md** - Project overview, architecture, build, test, and release workflow for developers.
- **Unit tests** - ResonanceBakeConfig, ResonanceSceneUtils, ResonancePlayerConfig, sample_rate_override.

### Fixed

- **IPL error handling** - All `ipl*Create` calls now check `IPL_STATUS_SUCCESS`, log via ResonanceLog, and perform proper cleanup on failure (ResonanceGeometry, ResonanceMixerProcessor, ResonanceReflectionProcessor).
- **SOFA asset null checks** - HRTF init validates `hrtf_sofa_asset` and `get_data_ptr()`/`get_size()` before use; falls back to default HRTF on invalid data.
- **ResonancePlayer reverb buffer** - Stack buffer size increased to support frame size 2048 (was fixed at 512).

### Changed

- **ResonanceLogger integration** - C++ `ResonanceLog::error` and `ResonanceLog::warn` now forward to ResonanceLogger when available.
- **GDScript typing** - Added type hints to plugin callbacks, ResonanceSceneUtils, ResonanceRuntime.
- **ResonanceServerConfig** - Accepts frame size 2048; sample rate from config (supports override).

## [0.8.0] - 2025-02-24

**First public release.** Nexus Resonance brings Steam Audio (Phonon) integration to Godot 4 with physics-based spatial audio.

### Added

**Nodes & Resources**

- ResonanceProbeVolume - baked reverb and pathing data; probe visualization
- ResonanceGeometry / ResonanceDynamicGeometry - acoustic geometry for static and moving meshes
- ResonanceStaticScene - container for exported static scene assets
- ResonancePlayer - 3D audio source with occlusion, reverb, pathing (replaces AudioStreamPlayer3D)
- ResonanceListener - listener position for debug visualization
- ResonanceRuntime + ResonanceRuntimeConfig - listener updates, quality settings
- ResonanceBakeConfig - per-volume bake settings (reflections, pathing, quality)
- ResonancePlayerConfig - per-source settings (distance, occlusion, directivity, pathing, perspective)
- ResonanceMaterial - absorption, scattering, transmission; 16 presets (concrete, wood, metal, glass, brick, drywall, plaster, carpet, fabric, rubber, plastic, ceramic, marble tile, acoustic ceiling tile, gravel/sand, water)
- ResonanceSOFAAsset - custom HRTF from SOFA files for binaural playback

**Audio Features**

- HRTF spatialization with optional SOFA support
- Occlusion - raycast or volumetric model
- Reflections - baked (Convolution/Parametric/Hybrid) and realtime (0-8192 rays); optional Radeon Rays GPU acceleration
- Pathing - multi-path sound propagation (baked)
- Transmission - frequency-dependent/independent sound through walls
- Directivity - source directivity patterns
- Air absorption - distance-based attenuation
- Perspective correction - third-person spatialization adjustment
- ResonanceAudioEffect - reverb bus effect (auto-configured)

**Editor**

- Toolbar (Tools → Nexus Resonance): Export Static Scene, Export Dynamic Meshes, Bake All Probe Volumes, Clear Probe Batches, Unlink Probe Volume References
- Probe volume inspector: bake workflow, prerequisites, optional probe batch cleanup (remove probes or baked layers)
- Probe sphere gizmo for volume visualization
- SOFA importer for HRTF files
- Configurable bake parameters in Project Settings (`nexus/resonance/bake`_*)

**Debug & Tooling**

- Debug overlay - runtime status, audio instrumentation, log
- Debug visualization - occlusion rays, reflection rays, path rays
- ResonanceLogger autoload for categorized logging

**Release & CI**

- Release workflow: addon ZIP, source ZIP, GitHub Releases with binaries
- Multi-platform CI (Linux, Windows, macOS), GUT and C++ tests, CodeQL

### Fixed

- Bake Pathing: skip geometry refresh before pathing bake to avoid Godot crash (IPL scene state conflict)
- Reflections bake: reload volumes using probe data (fixes multi-volume probe_data sync)

### Changed

- Bake parameters (num_rays, num_bounces, pathing) read from Project Settings
- Project structure: LICENSE and CHANGELOG at repo root; tests in `test/`


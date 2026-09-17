# Nexus Resonance - Agent Guide

Nexus Resonance is a Godot 4 addon that integrates Steam Audio (Phonon) for spatial audio: occlusion, reflections, reverb, pathing, and HRTF-based binaural rendering.

## Project Structure

```
nexus-resonance/
├── src/                    # C++ GDExtension (Steam Audio integration)
│   ├── resonance_*.cpp/h   # Core classes (Server, Player, Geometry, Baker, etc.)
│   ├── resonance_log.*     # C++ logging (ResonanceLog, forwards to ResonanceLogger)
│   ├── test/               # C++ unit tests (Catch2)
│   ├── lib/
│   │   ├── godot-cpp/      # Godot C++ bindings (submodule)
│   │   ├── catch2/         # Catch2 test framework (submodule, v2.x)
│   │   ├── pffft/          # FFT library for iOS (submodule)
│   │   ├── libmysofa/      # HRTF/SOFA reader for iOS (submodule)
│   │   └── steamaudio/     # Steam Audio Phonon SDK (downloaded via install script)
│   └── register_types.cpp  # Module init
├── addons/nexus_resonance/ # Addon source of truth (GDScript + GDExtension)
│   ├── plugin.gd           # EditorPlugin
│   ├── scripts/            # GDScript helpers (ResonanceRuntimeBaker, ResonanceRuntimeExporter, configs)

│   ├── editor/             # Bake runner, inspectors, gizmos
│   ├── bin/                # Built .dll/.so/.dylib/.a (GDExtension)
│   └── doc_classes/        # API docs (XML)
├── project/                # Local Godot test project (gitignored; prefer junction, see below)
├── Makefile                # Cross-platform build targets
└── SConstruct              # SCons build
```



## Local addon link (Windows)

Prefer a directory junction so Godot and the IDE share one tree (avoids Cursor/LSP `Class "…" hides a global script class` when both `addons/` SoT and `project/addons/` copies define the same `class_name`):

```powershell
# Close Godot first if DLLs under project/addons are locked
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/link_addon_into_project.ps1
```

- After a successful link, edits under `addons/nexus_resonance/` are visible in Godot immediately; do not copy-overwrite the junction.
- Tracked GUT scripts live under repo-root `test/`; Godot reads `project/test/`. After changing tests, copy `test/` into `project/test/` (or run `.github/scripts/prepare_godot_ci_project.sh` only when there is no addon junction).
- CI may still copy via `.github/scripts/prepare_godot_ci_project.sh` (do not run that locally over a junction).
- For remaining public `class_name` Resources (`ResonancePlayerConfig`, etc.): open the script via `project/addons/nexus_resonance/...` in the editor so the LSP path matches Godot `res://`. Opening only the repo-root SoT path while Godot is attached can still show a hide warning for those few globals.



## Architecture

- **Editor plugin vs GDExtension:** Toggling the addon under **Project Settings → Plugins** only affects `[plugin.gd](../addons/nexus_resonance/plugin.gd)`. Native classes and `ResonanceServer` come from the **.gdextension** and stay loaded until editor restart. See **[EDITOR_PLUGIN_VS_GDEXTENSION.md](EDITOR_PLUGIN_VS_GDEXTENSION.md)**.
- **ResonanceServer** (C++): Singleton, owns Steam Audio context, simulator, scene, HRTF.
- **ResonanceRuntime** (GDScript): Scene node, drives init, listener fallback, reverb bus.
- **ResonanceRuntimeConfig** (GDScript): Resource with all runtime settings (sample rate, frame size, reflection type, etc.).
- **ResonancePlayer** / **ResonanceAmbisonicPlayer** (C++): Audio sources with direct, reverb, pathing.
- **ResonancePlayer architecture (ASP3D + Hide):** `ResonancePlayer` **extends** `AudioStreamPlayer3D` (Godot transport: stream, play/stop, volume, pitch, polyphony, transform). Steam ownership lives on `ResonancePlayerConfig` (distance, occlusion, directivity, reflections, pathing, buses). Godot has no spatializer-plugin hook, so Nexus fuses host playback and Steam Source via inheritance + config. **Do not** rebase onto bare `Node3D` - clarity comes from property ownership and inspector hide, not from dropping the engine player. With `player_config` set, `_validate_property` hides inert ASP3D spatial knobs; `_apply_steam_mode_asp3d_guards` forces `ATTENUATION_DISABLED` and clears Godot `max_distance` (so saved ASP3D values cannot double-attenuate or sphere-cull). Without config, the node is plain `AudioStreamPlayer3D`. Same pattern: `ResonanceAmbisonicPlayer` extends `AudioStreamPlayer` and hides incompatible props.
- **ResonancePlayer polyphony:** When `player_config` is set, Godot may run several `ResonanceInternalPlayback` instances (`max_polyphony` > 1). The node keeps a mutex-protected registry; `_process` **broadcasts** the same `PlaybackParameters` (and shared `source_handle`) to every active voice so none fall back to dry passthrough. **Reverb split** sums `read_reverb_frames` across all registered voices (with clamp). `ResonancePlayer::~` orphans playbacks so their destructors never touch a freed owner. `ResonanceServer::record_convolution_feed` is instrumentation-only (tracks min/max gain/RMS); convolution still runs per voice via the reflection mixer - multiple feeds per frame from overlapping voices are normal.
- **ResonancePlayer vs** `AudioStreamPlayer3D` **(with** `player_config`**):**
  - `**set_stream` / `get_stream`:** Overridden so the serialized/inspector stream is the user’s `AudioStream`; the engine plays `ResonanceInternalStream`. Clearing `player_config` unwraps to the plain parent stream slot.
  - **Volume:** `owner_effective_volume_linear` applies `min(volume_db, max_db)` → linear to the dry decoder buffer **before** Steam DSP (source loudness; wet/convolution follow). Dry-only level: `direct_mix_level`. Godot does not apply node volume to GDExtension `_mix` output.
  - **Ignored for distance (use** `ResonancePlayerConfig`**):** Godot `attenuation_model`, `unit_size`, `max_distance` - forced disabled/cleared when config is set (`_ready` and `set_player_config`).
  - **Hidden / inert (Steam replaces):** `emission_angle`*, `attenuation_filter`*, `panning_strength`, `doppler_tracking`, `area_mask`, `playback_type`, inspector `bus` (routing via config / `ResonanceRuntime`).
  - `**get_audio_instrumentation`:** Adds `godot_`* snapshot keys for the fields above plus pitch/max_db/unit_size.
  - **Reverb split child:** Separate `AudioStreamPlayer` bus; not auto-synced to parent bus/mute.
  - **Follow-ups (manual / future):** Verify `stream_paused` / `set_playing` with split child; verify `seek` across all polyphony voices if Godot only seeks one playback.
- **ResonanceGeometry** (C++): Mesh → Steam Audio scene (static/dynamic, asset or runtime).
- **ResonanceProbeVolume** (C++): Baked reverb/pathing probes.



## Steam Audio component surface (gap map)

Compare public Resonance APIs to Steam Audio's Source / Settings / Listener / Probe Batch surface (**4.8.1**). Godot keeps snake_case; "closer" means **component shape, fields, defaults, bake ownership** - not foreign C# names. Local `references/` is gitignored.

### Mapping (already close)


| Steam Audio concept | Nexus Resonance |
| ----------------- | --------------- |
| Source + host audio emitter | `ResonancePlayer` + `ResonancePlayerConfig` (occlusion / transmission / air absorption / directivity UserDefined, mix levels, pathing overrides, binaural overrides) |
| Settings | `ResonanceRuntimeConfig` (HRTF/SOFA, rays/bounces/duration/order, Hybrid, pathing vis*, `scene_type`, OpenCL, TAN as `reflection_type`) |
| Probe Batch | `ResonanceProbeVolume` + `ResonanceBakeConfig` |
| Geometry / Dynamic Object / Material | `ResonanceStaticGeometry` / `ResonanceDynamicGeometry` / `ResonanceMaterial` (`export_all_children` exists) |
| Ambisonic Source | `ResonanceAmbisonicPlayer` (`apply_hrtf`) |
| Manager `NotifyAudioListenerChanged*` | `ResonanceServer.notify_listener_changed` / `notify_listener_changed_to` (not documented on `ResonanceRuntime`) |


### Gaps (Steam Audio has it; Resonance missing or shaped differently)

#### A. Listener (largest form gap)

Steam Audio Listener: `applyReverb`, `reverbType` (Realtime/Baked), `currentBakedListener`, `useAllProbeBatches` / `probeBatches`, Bake button.

`ResonanceListener`: only `listener_valid`. Listener-centric reverb is driven by Runtime + ProbeVolume bake, not by the listener node.

#### B. Baked Source / Baked Listener as components

Steam Audio: dedicated nodes with `influenceRadius`, probe-batch list, Bake on the endpoint. Source has `currentBakedSource`.

Resonance: `bake_sources` / `bake_listeners` + one `bake_influence_radius` **on the volume**. Player has no `current_baked_source` slot (Static Source uses the player pose per PlayerConfig).

#### C. Source fields

Closed:

- `use_distance_curve_for_reflections` on `ResonancePlayerConfig` (default **false**; Linear/Curve only). Feeds Phonon CALLBACK into Reflections IR correction; Pathing always gets CALLBACK for Linear/Curve on path length. No Euclidean mix multiply on wet (PATH-H01).
- `distance_attenuation` master toggle (default **true**). Inverse ignores max (grayed out); Linear/Curve use min/max.
- `occlusion_radius` inspector alias of `source_radius`.
- `ResonancePlayer.set_inputs` / `get_outputs` wrappers (no IPL handles to GDScript).

Attenuation mode is one TYPE for Direct/Pathing/Reflections; evaluated distance differs (Euclid vs path length vs IR delay).

#### D. Settings / bake quality

- No global `hrtfDisabled` (only per-path binaural + `use_virtual_surround`).
- No `defaultMaterial` on RuntimeConfig.
- Bake rays/bounces/threads live on **BakeConfig**, not Settings; no separate `bakingDuration` / `bakingIrradianceMinDistance` / bake CPU %.
- No independent `bakeConvolution` + `bakeParametric` (single `reflection_type`).
- TAN overrides (`TANDuration`, `TANAmbisonicOrder`, `TANMaxSources`), OpenCL CU reservation, `bakingBatchSize` missing or incomplete on Settings.
- Steam Audio realtime rays default often 4096 (floor 1024); Resonance default **Off (0)** with smaller steps - intentional for Godot, but different.
- Ambisonic order Steam Audio 0-3; Resonance 1-3.

#### E. Mixer

Steam Audio Mixer Return (pull indirect off the source path; Convolution/TAN). Resonance: bus + `ResonanceAudioEffect` + parametric split. No Mix-Return analog.

#### F. Reverb Data Point

Steam Audio: bake Convolution+Parametric IR/Energy into an asset (read API). Resonance: **analysis marker** only; no runtime IR asset.

#### G. Geometry

No `terrainSimplificationLevel` (Godot has no dedicated terrain mesh exporter; mesh export only).

#### H. Defaults (API feel, not missing fields)

Steam Audio Source is often **opt-in** (occlusion/reflections/airAbs/distanceAttenuation default false). Resonance PlayerConfig: occlusion/transmission/airAbs/reflections **default true**. Pathing `find_alternate_paths`: Steam Audio true, Resonance false.

### Resonance ahead / Godot-only

`ResonanceRuntimeBaker` / `ResonanceRuntimeExporter`, `GEN_VOLUME`, `ResonanceProbeExclusion`, Custom Godot Physics, adaptive reflection budget, separate sim intervals, ASP3D inheritance, bus routing, experimental FMOD/Coda.

### Suggested phases (future work)

1. **Listener form:** `apply_reverb`, `reverb_type`, `current_baked_listener` (or NodePath).
2. **Baked Source/Listener nodes** + Player `current_baked_source`; keep or deprecate volume bake arrays as scan-fill.
3. **Source fields:** done (Gap C) - `use_distance_curve_for_reflections`, `distance_attenuation`, `occlusion_radius`, Player `set_inputs` / `get_outputs`.
4. **Settings surface:** `hrtf_disabled`, `default_material`, bake duration/irradiance on RuntimeConfig (BakeConfig stays override).
5. **Mix Return** only if Convolution/TAN workflows need it.
6. ReverbDataPoint asset bake only if designers need IR export.

Discoverability: document `notify_listener_changed*` on `ResonanceRuntime` as well (Manager-level API).



## Build

```bash
# Fetch submodules
git submodule update --init --recursive

# Install Steam Audio SDK
python3 scripts/install_steam_audio.py

# Build for current platform
scons

# Platform-specific builds via Makefile
make build-windows    # Windows x64 (cross-compile with mingw)
make build-linux      # Linux x64
make build-macos      # macOS (universal)
make build-android    # Android arm64 + x86_64
make build-ios        # iOS arm64 (macOS only; builds pffft/libmysofa deps)
```

Output: `addons/nexus_resonance/bin/`

## Test

Unit tests (GUT) live in tracked `[test/unit/](../test/unit/)` and are synced into `project/test/` by `[.github/scripts/prepare_godot_ci_project.sh](../.github/scripts/prepare_godot_ci_project.sh)` (or copy locally before running):

- `test_resonance_player_asp3d_params.gd` - Exposed `AudioStreamPlayer3D` knobs on ResonancePlayer (`volume_db` / `max_db` source loudness pre-Steam, `pitch_scale`, `playing`, `autoplay`, `stream_paused`, `max_polyphony`)
- `test_resonance_config.gd` - ResonanceRuntimeConfig (if present)
- `test_probe_data_loader.gd` / `test_probe_data_saver.gd` - Probe data (if present)
- `test_resonance_bake_settings.gd` - Bake settings (if present)
- `test_resonance_player_polyphony.gd` - ResonancePlayer `max_polyphony` / instrumentation API (if present)

Run via Godot with GUT addon or CLI (`project/run_tests.ps1` after sync).

**Manual regression (geometry teardown):** No C++ unit test covers full Phonon teardown. After changes to `ResonanceGeometry` cleanup, verify in a running project: attach **ResonanceDynamicGeometry** under a `MeshInstance3D`, run the game, then **change scene** (`change_scene_to_file` / equivalent). It must not crash; dynamic meshes are removed from the object’s `sub_scene` before `iplSceneRelease`, not from the global scene.

**Manual / GUT regression (dynamic spawn cutout):** Continuous tone must not hard-mute when spawning `ResonanceDynamicGeometry` into a non-empty scene. Scene: `[test/manual/dynamic_geometry_spawn_audio/](../test/manual/dynamic_geometry_spawn_audio/)` (see its README). GUT twin: `[test/unit/test_dynamic_geometry_spawn_keeps_spatial_ready.gd](../test/unit/test_dynamic_geometry_spawn_keeps_spatial_ready.gd)`.

## Release Workflow

1. Update version in `src/resonance_constants.h` (NEXUS_RESONANCE_VERSION).
2. Tag: `git tag v0.8.1`
3. Push tag: triggers `.github/workflows/release.yml` which builds all platforms (Linux, Windows, macOS, Android, iOS) and creates a GitHub Release with a unified addon zip.



## Pre Push / PR (Checklist)


| Was                           | Wie                                                                                                                                                                                                         |
| ----------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **C++-Format (wie Linux-CI)** | `[scripts/check_clang_format.sh](scripts/check_clang_format.sh)` - bei Fehlern `[scripts/format_cpp.sh](scripts/format_cpp.sh)`, dann erneut prüfen.                                                        |
| **Alles in einem (lokal)**    | `[scripts/prep_push.sh](scripts/prep_push.sh)` (Bash / Git Bash) oder `[scripts/prep_push.ps1](scripts/prep_push.ps1)` (PowerShell; `CLANG_FORMAT_BIN` oder `.tools\LLVM-14-extract\bin\clang-format.exe`). |
| **C++-Unit-Tests**            | `scons` (baut Tests standardmäßig), dann `build/tests/nexus_resonance_tests` bzw. `.exe`.                                                                                                                   |
| **GDScript**                  | GUT läuft in **tests.yml** auf geänderten Pfaden; bei relevanten `.gd`-Änderungen lokal GUT ausführen oder auf CI vertrauen.                                                                                |
| **Version sichtbar**          | Bei Release: `CHANGELOG` + `NEXUS_RESONANCE_VERSION` in `src/resonance_constants.h`.                                                                                                                        |


Cursor: Regel `[.cursor/rules/before-push.mdc](.cursor/rules/before-push.mdc)` - Assistent soll bei „will pushen“ / Release-Vorhaben Format-Check und C++-Tests ausführen.

## CI/CD

Cost-aware defaults: PR gates stay on Linux; push/merge to main does not re-run Tests; full multi-platform and CodeQL are manual.

- **tests.yml** - Path-filtered **PR** gate (no run for materials/icons/docs-only; **not** on push/merge to main). **Linux C++** (format + Catch2 + strict clang-tidy subset) when `src/`** changes. **Windows C++** and **full smoke suite** only via `workflow_dispatch` (`run_windows` / `run_all_smokes`). **Godot** (GUT + lightning smoke) when C++ or `.gd` changes. GDScript-only: restore cached Linux `.so` (rebuild only on cache miss). Concurrency cancels superseded runs.
- **gdscript-lint.yml** - `gdformat` / `gdlint` on changed `.gd` files only (PR + push).
- **build.yml** - Multi-platform GDExtension binaries (**manual** `workflow_dispatch` **only**). Use before a release if you need artifacts without tagging.
- **release.yml** - Full multi-platform build + GitHub Release on version tags (`v`*).
- **codeql.yml** - **Manual** `workflow_dispatch` **only** (needs Code Scanning / Advanced Security on the repo).
- **stale.yml** - Weekly stale issue/PR cleanup.



## Runtime debug overlays

Requires `ResonanceRuntime.enable_debug` at runtime (ignored in the editor). Hotkeys are configurable on `ResonanceRuntime`; defaults below.


| Key | Overlay                         | What it shows                                                                                                                                    |
| --- | ------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------ |
| F1  | Canvas HUD (`debug_overlay.gd`) | Server status, audio instrumentation per `ResonancePlayer`, reverb bus metrics. Alt+1-3 fold sections; Alt+R reset meters; Alt+A/B extra detail. |
| F2  | Performance overlay             | FPS / frame time (independent canvas).                                                                                                           |
| F3  | Player overlay                  | Single toggle: sets `ResonanceServer.debug_occlusion` and `debug_reflections`. Per-source 3D HUD label (occlusion, transmission, signal meters). |


`enable_debug` gates overlay **hotkeys** and hides canvas overlays when turned off. `ResonanceServer.debug_`* can still be set from scripts for in-game tooling without enabling F1-F3 (3D HUD follows those flags, not `enable_debug`). F3 is the supported way to turn the full player overlay on/off during development.

`scripts/debug_visualizer.gd` is a separate **Godot physics** line preview only; it is not part of the F3 Steam Audio overlay.

Historical CHANGELOG entries may mention older keys (e.g. F4 performance, F3-only canvas); current behavior is documented here and in `doc_classes/ResonanceRuntime.xml`.

## Baked reverb analysis

- **[ResonanceReverbDataPoint](addons/nexus_resonance/doc_classes/ResonanceReverbDataPoint.xml)** - analysis marker: Sample Reverb Here shows RT60 and energy (dB). Viewport gizmo; inherits `probe_data` from an ancestor ProbeVolume. Does not affect audio.
- `ResonanceServer.probe_data_query_baked_at_point` **/** `probe_data_query_baked_at_probe` - read `IPLEnergyField` via `iplProbeBatchGetEnergyField`, optional `iplProbeBatchGetReverb` (parametric RT60 triple), optional `IPLReconstructor` + `IPLImpulseResponse` preview when `reconstruct_ir` is true. Main-thread / editor tooling only; not wired into the audio mix path.
- Query dictionaries use `ok`, `total_energy`, `energy_q16`, nested `energy_field` / `impulse_response` packed arrays for plotting in GDScript.



## Runtime static geometry rebuild

Public API: `[ResonanceRuntimeExporter](../addons/nexus_resonance/doc_classes/ResonanceRuntimeExporter.xml)` (`class_name`) and `[ResonanceServer](../addons/nexus_resonance/doc_classes/ResonanceServer.xml)` export/replace methods. User-facing overview: [README](../README.md) (Runtime Static Rebuild). Helper script: `[resonance_runtime_exporter.gd](../addons/nexus_resonance/scripts/resonance_runtime_exporter.gd)`.

When a level uses a merged **ResonanceStaticScene** asset, live **ResonanceStaticGeometry** / **ResonanceGeometry** nodes are not registered in Phonon (the RSS pack owns the static mesh). Destroying or hiding geometry at runtime does not update Steam Audio until you rebuild and commit static packs.

```gdscript
wall.visible = false
await ResonanceRuntimeExporter.export_static_async(level_root, { "bake_probes": true })
```

`export_static` returns immediately; with `bake_probes` it still starts RAM bake after the deferred static reload (one frame). Prefer `export_static_async` when gameplay must wait for reload and bake. Caller-owned `opts.baker` is supported; await `baker.bake_finished` or use `export_static_async`.


| Workflow                                       | Steps                                                                                                                                                                                                                                                                   |
| ---------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Merged single RSS** (one pack for the level) | Hide/remove destructible meshes → `export_static_async(level_root, { "bake_probes": true })` (or `export_static` for fire-and-forget) → assigns in-memory asset to owned `ResonanceStaticScene`, deferred `reload`, then probe RAM bake against committed static packs. |
| **Per-destructible RSS sub-pack**              | Put destructible sections under their own `ResonanceStaticScene` with a pre-baked asset; `queue_free()` the node (or remove from tree) so `NOTIFICATION_EXIT_TREE` unregisters that pack via `remove_static_pack`. Other packs stay loaded.                             |
| **Movers / procedural meshes**                 | Use **ResonanceDynamicGeometry** (or dynamic geometry nodes), not static re-export.                                                                                                                                                                                     |
| **Ephemeral asset list (no RSS nodes)**        | `ResonanceRuntimeExporter.replace_static(assets, transforms)` (C++ `ResonanceServer.replace_static_scenes_from_assets`).                                                                                                                                                |


**C++ APIs (play mode):**

- `ResonanceServer.export_static_scene_to_geometry_asset(scene_root)` - in-memory `ResonanceGeometryAsset` (preferred for runtime).
- `ResonanceServer.export_static_scene_to_asset(scene_root, path)` - same merge, written to disk (`user://` is fine).
- `ResonanceServer.replace_static_scenes_from_assets(assets, transforms)` - clear all static packs, then load listed assets (no-op with warning when `scene_type` is Custom / Godot Physics).

**Residuals / limits:**

- No per-triangle or partial mesh removal without a full re-export of the affected export root.
- In-place material tweaks on deserialised RSS packs do not apply (`iplStaticMeshSetMaterial` on loaded packs); re-export after material changes.
- Editor static export menus may stay blocked during play; runtime helpers and C++ export are not play-blocked.
- Optional `optional_save_path` on the helper persists the asset for debugging; gameplay can stay fully in RAM.



## Pathing deviation from GDScript

- `ResonanceServer.set_pathing_deviation_callable(callable, samples_per_band)` - builds a per-band LUT from `callable(angle_rad, band) -> float` (0..1). The Callable runs on the main thread while **no** `simulation_mutex` is held; only `_pathing_deviation_mutex` is used briefly to copy/store the Callable before sampling. Simulation reads the LUT from the Steam deviation callback (worker thread).
- **Do not** call back into `ResonanceServer` (or other code that locks `simulation_mutex`) from inside the Callable while `set_pathing_deviation_callable` is running - that can deadlock if locks were held across the script call (Nexus avoids that; keep Callable side-effect free anyway).
- `clear_pathing_deviation_callback()` - back to default UTD (`nullptr` callback).
- C++ extensions can still call `set_pathing_deviation_callback(IPLDeviationCallback, userData)` directly; that clears any Callable LUT.



## Known Limits and Workarounds

- **ResonancePlayer sounds dry (no reverb/occlusion)** - Often was: child `ResonancePlayer` `_ready` ran before parent `ResonanceServer` init, so `create_source_handle` never ran again. Fixed by retrying handle creation from `play()` / `_process` when the server becomes ready. If you still hear dry audio, check `ResonanceServer.is_simulating()`, probe batches / geometry, and mix levels-not only `_ready` ordering.
- **Editor shutdown**: Do not call `ResourceSaver.remove_resource_format_saver` / `ResourceLoader.remove_resource_format_loader` in plugin `_exit_tree`; Godot may tear these down before the plugin, causing SIGSEGV.
- **GDExtension unload / plugin disable**: Do **not** call `clear_probe_batches` in `_disable_plugin`. Wiping the native registry while `ResonanceProbeVolume` nodes still hold `probe_batch_handle` values leaves stale handles after re-enable. Clear batches only via Project → Tools → Nexus Resonance → Clear Probe Batches when intentional.
- **Probe volume deletion**: Probe Volume clears refs on EXIT_TREE; ResonancePlayer auto-clears `pathing_probe_volume` when the target node is gone. If the error still occurs, use Tools > Unlink Probe Volume References before deleting.



## Coding Conventions

- **GDScript**: See `.cursor/rules/gdscript-nexus.mdc` - `@export_group`, `@export_enum`, Setter mit `_warn_restart()`.
- **C++**: See `.cursor/rules/cpp-gdextension.mdc` - GDExtension patterns, `CLASS_BINDING`.
- **Errors**: Use `ResonanceLog::error()` / `ResonanceLog::warn()` in C++; these forward to ResonanceLogger when available.
- **IPL calls**: Always check `IPL_STATUS_SUCCESS`; on failure log and cleanup.



## Audio Processor Pattern

All Steam Audio processors (Direct, Reflection, Path, Mixer, Ambisonic) follow a consistent pattern:

**Initialization order (Create)**

1. Context and config (sample rate, frame size, ambisonic order).
2. IPL effect objects (e.g. `iplDirectEffectCreate`, `iplBinauralEffectCreate`).
3. Buffers (`iplAudioBufferAllocate`).
4. Set `InitFlags` per successful step.

**Release order (Reverse of create)**

1. Release effect objects.
2. Free buffers (requires context).
3. Clear context reference.
4. Reset `InitFlags` to `NONE`.

**InitFlags**

- Bitwise enum per processor (e.g. `DirectInitFlags`, `AmbisonicInitFlags`).
- `process()` only runs when all required flags are set; avoids partial-init crashes.
- On failure, processors support passthrough fallback (Direct, Ambisonic) instead of silence.

**Process guards**

- Null checks at process entry: context, input/output buffers.
- InitFlags guard before processing.
- Processors may return early with passthrough or silence on invalid state.

**Double-buffering (audio thread)**

- Listener coordinates, parametric reverb cache, HRTF, ReflectionMixer use main-write / audio-read double-buffers.
- Atomic flags trigger swap on consume; lock-free for the audio hot path.



## Key Files for Common Tasks


| Task                      | Files                                                                                                         |
| ------------------------- | ------------------------------------------------------------------------------------------------------------- |
| Add runtime config option | `resonance_runtime_config.gd`, `resonance_server_config.cpp/h`                                                |
| Add Steam Audio feature   | `resonance_server*.cpp`, `resonance_server.h`, `resonance_player.cpp`, processors                             |
| FMOD event emitter node   | `resonance_fmod_event_emitter.cpp/h`, `resonance_fmod_bridge.cpp/h`, `docs/FMOD_BRIDGE.md`                    |
| Coda event emitter node   | `resonance_coda_event_emitter.cpp/h`, `resonance_coda_bridge.gd`                                              |
| Editor UI                 | `plugin.gd`, `editor/resonance_*.gd`                                                                          |
| Bake pipeline             | `resonance_baker.cpp`, `editor/resonance_bake_runner.gd`                                                      |
| Runtime static rebuild    | `resonance_runtime_exporter.gd`, `ResonanceServer` export/replace, `doc_classes/ResonanceRuntimeExporter.xml` |
| Native node migration     | [docs/adr/001-native-resonance-node-migration.md](adr/001-native-resonance-node-migration.md)                 |



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
│   ├── scripts/            # GDScript helpers
│   ├── editor/             # Bake runner, inspectors, gizmos
│   ├── bin/                # Built .dll/.so/.dylib/.a (GDExtension)
│   └── doc_classes/        # API docs (XML)
├── project/                # Local Godot test project (gitignored; sync addon here)
├── Makefile                # Cross-platform build targets
└── SConstruct              # SCons build
```

## Architecture

- **Editor plugin vs GDExtension:** Toggling the addon under **Project Settings → Plugins** only affects [`plugin.gd`](../addons/nexus_resonance/plugin.gd). Native classes and `ResonanceServer` come from the **.gdextension** and stay loaded until editor restart. See **[EDITOR_PLUGIN_VS_GDEXTENSION.md](EDITOR_PLUGIN_VS_GDEXTENSION.md)**.
- **ResonanceServer** (C++): Singleton, owns Steam Audio context, simulator, scene, HRTF.
- **ResonanceRuntime** (GDScript): Scene node, drives init, listener fallback, reverb bus.
- **ResonanceRuntimeConfig** (GDScript): Resource with all runtime settings (sample rate, frame size, reflection type, etc.).
- **ResonancePlayer** / **ResonanceAmbisonicPlayer** (C++): Audio sources with direct, reverb, pathing.
- **ResonancePlayer architecture (ASP3D + Hide):** `ResonancePlayer` **extends `AudioStreamPlayer3D`** (Godot transport: stream, play/stop, volume, pitch, polyphony, transform). Steam ownership lives on **`ResonancePlayerConfig`** (distance, occlusion, directivity, reflections, pathing, buses). This mirrors Steam Audio Unity (`AudioSource` + companion `Steam Audio Source`); Godot has no spatializer-plugin hook, so Nexus fuses both roles via inheritance + config. **Do not** rebase onto bare `Node3D` - clarity comes from property ownership and inspector hide, not from dropping the engine player. With `player_config` set, `_validate_property` hides inert ASP3D spatial knobs; `_apply_steam_mode_asp3d_guards` forces `ATTENUATION_DISABLED` and clears Godot `max_distance` (so saved ASP3D values cannot double-attenuate or sphere-cull). Without config, the node is plain `AudioStreamPlayer3D`. Same pattern: `ResonanceAmbisonicPlayer` extends `AudioStreamPlayer` and hides incompatible props.
- **ResonancePlayer polyphony:** When `player_config` is set, Godot may run several `ResonanceInternalPlayback` instances (`max_polyphony` > 1). The node keeps a mutex-protected registry; `_process` **broadcasts** the same `PlaybackParameters` (and shared `source_handle`) to every active voice so none fall back to dry passthrough. **Reverb split** sums `read_reverb_frames` across all registered voices (with clamp). `ResonancePlayer::~` orphans playbacks so their destructors never touch a freed owner. `ResonanceServer::record_convolution_feed` is instrumentation-only (tracks min/max gain/RMS); convolution still runs per voice via the reflection mixer - multiple feeds per frame from overlapping voices are normal.
- **ResonancePlayer vs `AudioStreamPlayer3D` (with `player_config`):**
  - `**set_stream` / `get_stream`:** Overridden so the serialized/inspector stream is the user’s `AudioStream`; the engine plays `ResonanceInternalStream`. Clearing `player_config` unwraps to the plain parent stream slot.
  - **Volume:** `owner_effective_volume_linear` applies `min(volume_db, max_db)` → linear to the dry decoder buffer **before** Steam DSP (source loudness; wet/convolution follow). Dry-only level: `direct_mix_level`. Godot does not apply node volume to GDExtension `_mix` output.
  - **Ignored for distance (use `ResonancePlayerConfig`):** Godot `attenuation_model`, `unit_size`, `max_distance` - forced disabled/cleared when config is set (`_ready` and `set_player_config`).
  - **Hidden / inert (Steam replaces):** `emission_angle*`, `attenuation_filter*`, `panning_strength`, `doppler_tracking`, `area_mask`, `playback_type`, inspector `bus` (routing via config / `ResonanceRuntime`).
  - `**get_audio_instrumentation`:** Adds `godot_`* snapshot keys for the fields above plus pitch/max_db/unit_size.
  - **Reverb split child:** Separate `AudioStreamPlayer` bus; not auto-synced to parent bus/mute.
  - **Follow-ups (manual / future):** Verify `stream_paused` / `set_playing` with split child; verify `seek` across all polyphony voices if Godot only seeks one playback.
- **ResonanceGeometry** (C++): Mesh → Steam Audio scene (static/dynamic, asset or runtime).
- **ResonanceProbeVolume** (C++): Baked reverb/pathing probes.

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

Unit tests (GUT) live in tracked [`test/unit/`](../test/unit/) and are synced into `project/test/` by [`.github/scripts/prepare_godot_ci_project.sh`](../.github/scripts/prepare_godot_ci_project.sh) (or copy locally before running):

- `test_resonance_player_asp3d_params.gd` - Exposed `AudioStreamPlayer3D` knobs on ResonancePlayer (`volume_db` / `max_db` source loudness pre-Steam, `pitch_scale`, `playing`, `autoplay`, `stream_paused`, `max_polyphony`)
- `test_resonance_config.gd` - ResonanceRuntimeConfig (if present)
- `test_probe_data_loader.gd` / `test_probe_data_saver.gd` - Probe data (if present)
- `test_resonance_bake_settings.gd` - Bake settings (if present)
- `test_resonance_player_polyphony.gd` - ResonancePlayer `max_polyphony` / instrumentation API (if present)

Run via Godot with GUT addon or CLI (`project/run_tests.ps1` after sync).

**Manual regression (geometry teardown):** No C++ unit test covers full Phonon teardown. After changes to `ResonanceGeometry` cleanup, verify in a running project: attach **ResonanceDynamicGeometry** under a `MeshInstance3D`, run the game, then **change scene** (`change_scene_to_file` / equivalent). It must not crash; dynamic meshes are removed from the object’s `sub_scene` before `iplSceneRelease`, not from the global scene.

**Manual / GUT regression (dynamic spawn cutout):** Continuous tone must not hard-mute when spawning `ResonanceDynamicGeometry` into a non-empty scene. Scene: [`test/manual/dynamic_geometry_spawn_audio/`](../test/manual/dynamic_geometry_spawn_audio/) (see its README). GUT twin: [`test/unit/test_dynamic_geometry_spawn_keeps_spatial_ready.gd`](../test/unit/test_dynamic_geometry_spawn_keeps_spatial_ready.gd).

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

- **tests.yml** - Path-filtered **PR** gate (no run for materials/icons/docs-only; **not** on push/merge to main). **Linux C++** (format + Catch2 + strict clang-tidy subset) when `src/**` changes. **Windows C++** and **full smoke suite** only via `workflow_dispatch` (`run_windows` / `run_all_smokes`). **Godot** (GUT + lightning smoke) when C++ or `.gd` changes. GDScript-only: restore cached Linux `.so` (rebuild only on cache miss). Concurrency cancels superseded runs.
- **gdscript-lint.yml** - `gdformat` / `gdlint` on changed `.gd` files only (PR + push).
- **build.yml** - Multi-platform GDExtension binaries (**manual `workflow_dispatch` only**). Use before a release if you need artifacts without tagging.
- **release.yml** - Full multi-platform build + GitHub Release on version tags (`v*`).
- **codeql.yml** - **Manual `workflow_dispatch` only** (needs Code Scanning / Advanced Security on the repo).
- **stale.yml** - Weekly stale issue/PR cleanup.

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


| Task                      | Files                                                                             |
| ------------------------- | --------------------------------------------------------------------------------- |
| Add runtime config option | `resonance_runtime_config.gd`, `resonance_server_config.cpp/h`                    |
| Add Steam Audio feature   | `resonance_server*.cpp`, `resonance_server.h`, `resonance_player.cpp`, processors |
| FMOD event emitter node   | `resonance_fmod_event_emitter.cpp/h`, `resonance_fmod_bridge.cpp/h`, `docs/FMOD_BRIDGE.md` |
| Coda event emitter node   | `resonance_coda_event_emitter.cpp/h`, `resonance_coda_bridge.gd` |
| Editor UI                 | `plugin.gd`, `editor/resonance_*.gd`                                              |
| Bake pipeline             | `resonance_baker.cpp`, `editor/resonance_bake_runner.gd`                          |
| Native node migration     | [docs/adr/001-native-resonance-node-migration.md](adr/001-native-resonance-node-migration.md) |



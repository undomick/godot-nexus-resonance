# Nexus Resonance

[![Discord](https://img.shields.io/discord/1446024019341086864?label=Discord&logo=discord&style=flat-square&color=5865F2)](https://discord.gg/VTSpAEHHhW)
[![Ko-fi](https://img.shields.io/badge/Support%20me-Ko--fi-F16061?style=flat-square&logo=ko-fi&logoColor=white)](https://ko-fi.com/jundrie)

Steam Audio integration for Godot 4: physics-based occlusion, reverb, and pathing.

## Quick Start

1. **Enable the plugin** in Project → Project Settings → Plugins → Resonance.

2. **Add a ResonanceProbeVolume** to your scene. Place it where reverb should be sampled (e.g. room center).

3. **Add ResonanceGeometry** as child of each MeshInstance3D that should affect audio (walls, floor, ceiling).

4. **Assign ResonanceMaterial** to each geometry (use presets from `addons/nexus_resonance/materials/`).

5. **Bake Probes**: Select the ResonanceProbeVolume → use "Bake Probes" in the 3D viewport toolbar.

6. **Replace AudioStreamPlayer3D with ResonancePlayer** and assign a **ResonancePlayerConfig** on `player_config` for sources that need occlusion/reverb. With a config, Steam owns distance/spatialization; Godot’s 3D attenuation knobs are hidden. Without a config, ResonancePlayer behaves like a plain AudioStreamPlayer3D.

7. **ResonanceRuntime** node handles listener updates and init. Assign a **ResonanceRuntimeConfig** resource for quality settings. Structure is aligned with Steam Audio Settings for compatibility.

## Bake Workflow

1. **Bake Probes (Reflections)** - Required first. Samples reverb at probe positions using `ResonanceRuntime.reflection_type` and bake Ambisonic Order (`ResonanceRuntimeConfig.bake_ambisonic_order`, overridable per volume on `ResonanceBakeConfig.bake_ambisonics_order`: Use Global / 1st–3rd). Playback uses Realtime Ambisonic Order separately. Pathing bake follows Runtime `pathing_enabled`. Requires ResonanceGeometry on MeshInstance3Ds. Saves to the Probe Data resource on the volume. Optional: add **ResonanceProbeExclusion** children under the volume to skip probes inside those boxes.
2. **Bake Pathing** - Optional. Enables multi-path sound around obstacles. Run after Bake Probes.
3. **Bake Static Source / Static Listener** - Optional. For static sound sources or listener positions. Set `scan_targets` to scene-tree roots (nodes or instanced scenes), then use inspector **Update Targets** to fill `bake_sources` / `bake_listeners` (replaces those arrays). Or assign NodePaths manually. Non-empty arrays enable the corresponding bake pass (no separate Additional Bake checkbox). At runtime use `add_bake_source` / `remove_bake_source` and the listener equivalents.

Use the toolbar buttons (Bake Probes, Bake Pathing, Bake More) when a ResonanceProbeVolume is selected. Bake progress appears in the toolbar.

### Runtime Baking (Procedural Generation)
If you are generating levels dynamically or need to bake acoustics in an exported game without the Godot Editor UI, you can use the `ResonanceRuntimeBaker` public API. This bypasses the Editor's disk-write requirements and bakes acoustic data directly into RAM.

```gdscript
var runtime_baker = ResonanceRuntimeBaker.new()

# Optional: Connect to signals for custom loading screens
runtime_baker.bake_progress_updated.connect(func(msg): print("Baking: ", msg))
runtime_baker.bake_finished.connect(_on_bake_finished)

# Pass your array of ResonanceProbeVolumes and the root of your procedural level
runtime_baker.bake_volumes_to_ram([my_probe_volume], map_root_node)

# Optional: Flush and reload runtime bakes (RAM only)
runtime_baker.flush_volumes([my_probe_volume])
runtime_baker.reload_volumes([my_probe_volume])

# Optional: Same, for all probe volumes under a scene root
runtime_baker.flush_all_runtime_bakes(map_root_node)
runtime_baker.reload_all_runtime_bakes(map_root_node)

# Clean up when you are completely finished
runtime_baker.shutdown()
```

### Runtime Static Rebuild (Destroyable / Streaming)

When a level uses a merged **ResonanceStaticScene**, live static geometry nodes are not registered in Phonon. Hiding or freeing walls does not update occlusion/reverb until you re-export and reload. Use the `ResonanceRuntimeExporter` public API from play mode / exported games. Hidden (`visible = false`) and freed nodes are omitted from the merge.

```gdscript
# Prefer await when gameplay must wait for reload (+ optional RAM probe bake)
wall.visible = false
await ResonanceRuntimeExporter.export_static_async(level_root, { "bake_probes": true })

# Fire-and-forget (returns immediately; with bake_probes, RAM bake starts next frame after reload)
# ResonanceRuntimeExporter.export_static(level_root, { "bake_probes": true })

# In-memory asset only (no assign / reload)
# var asset = ResonanceRuntimeExporter.export_asset(level_root)

# Full pack replace when there is no ResonanceStaticScene node
# ResonanceRuntimeExporter.replace_static(assets, transforms)

# Debounced ResonanceRuntime.request_static_scene_reload
# ResonanceRuntimeExporter.reload(level_root)

# opts keys: bake_probes, probe_volumes, baker (ResonanceRuntimeBaker),
# optional_save_path, static_scene_node, reload (default true)
```

Movers belong on **ResonanceDynamicGeometry**, not a static re-export. Custom scene type (Godot Physics) cannot replace static packs. Low-level C++ (prefer the helper): `ResonanceServer.export_static_scene_to_geometry_asset`, `export_static_scene_to_asset`, `replace_static_scenes_from_assets`.

## Audio Buffer & Latency

- **ResonanceRuntimeConfig → Ray Tracer Settings → Audio Frame Size**: Steam Audio processing block size (256, 512, 1024). 512 matches Godot’s typical mix callback. 256 = lower latency, more CPU; 1024 = higher latency, less CPU.
- **Project Settings → Audio → Driver → Output Latency**: Godot’s output buffer latency in ms. Lower values reduce latency but increase CPU load. Default in this project: 15 ms.

## Bake Parameters

Configure in Project Settings → Audio → Nexus Resonance:

- `bake_num_rays`, `bake_num_bounces` - reflection quality
- `bake_num_threads` - CPU threads for baking
- `bake_pathing_*` - pathing bake parameters (vis_range, path_range, num_samples, radius, threshold)

## Probe / Runtime Compatibility

Baked probe data must match the runtime reflection type exactly (same bake layer). Mismatched reflection types skip the whole Probe Volume at load and show red gizmos in the editor:

| baked_reflection_type | runtime reflection_type | Compatible? |
|-----------------------|--------------------------|-------------|
| 0 (Convolution)       | 0 (Convolution) or 3 (TAN) | Yes       |
| 1 (Parametric)        | 1 (Parametric)           | Yes         |
| 2 (Hybrid)            | 2 (Hybrid)               | Yes         |
| -1 (Legacy, both)     | 0, 1, 2, or 3            | Yes         |

Bake `reflection_type`, pathing on/off, and ambisonics order come from [ResonanceRuntime] (not BakeConfig). Changing them after a bake requires a rebake for an exact match.

**Ambisonics:** If bake order and runtime order differ, the batch still loads and playback uses `min(baked, runtime)` with one aggregated warning naming the volumes.

**Pathing:** Controlled by `ResonanceRuntimeConfig.pathing_enabled`. If pathing is on but the volume has no pathing layer, pathing is skipped (reflections keep working) and a warning lists the volumes. Pathing quality ranges stay on BakeConfig.

## Troubleshooting

| Issue | Solution |
|-------|----------|
| **"Bake Probes first"** | Pathing and static source/listener bakes require baked probe data. Run Bake Probes before Bake Pathing or Bake Static Source/Listener. |
| **"GDExtension not loaded"** | The native library (nexus_resonance.dll/.so/.dylib) is missing or incompatible. Ensure the GDExtension binaries in `addons/nexus_resonance/bin/` match your Godot version and platform. |
| **"Steam Audio Context/Scene missing"** | ResonanceGeometry nodes must be in the scene and refreshed. Add ResonanceGeometry as child of MeshInstance3Ds, assign ResonanceMaterial, then try Bake Probes again. |
| **Probes not visible** | Select the ResonanceProbeVolume and enable the "Viz" toggle in the toolbar. Ensure the GDExtension is loaded. |
| **Bake fails / no output** | Check the Godot Output/Console for Steam Audio errors. Ensure scene geometry has ResonanceGeometry with valid materials. |
| **Probe batch rejected / no reverb** | Baked reflection type must match runtime (see Probe/Runtime Compatibility). |
| **Pathing silent / Ambisonics warning** | Enable pathing on ResonanceRuntime and rebake pathing, or match Bake Ambisonic Order (Runtime / BakeConfig) to the baked data and rebake. Realtime Ambisonic Order is playback-only. |
| **Debug Reflection Rays** | Enable **Realtime Rays** (64+) in ResonanceRuntimeConfig and **Debug Reflections** in the config. Add a ResonanceListener (e.g. under Camera3D). Geometry from ResonanceGeometry or re-exported static scenes is used for ray viz. |

## Requirements

- Godot 4.6 (or compatible 4.x)
- Steam Audio (Phonon) - bundled with the GDExtension

## Developing

Before you open a PR, build with unit tests enabled and run the Catch2 binary (this matches what CI runs on Linux and Windows):

```bash
# Linux / macOS
scons platform=linux target=editor build_tests=1 -j$(nproc)
./build/tests/nexus_resonance_tests

# Windows (e.g. Git Bash or MSVC environment)
scons platform=windows target=editor build_tests=1
./build/tests/nexus_resonance_tests.exe
```

When you fix a bug in native code, add a small focused test under `src/test/` if you can check the behavior without loading the full Godot editor (follow the existing `test_*.cpp` patterns).

## Support & Community

Join the [Discord server](https://discord.gg/VTSpAEHHhW) to ask questions, suggest features, or show off your projects made with this addon.

## License & Dependencies

- **Nexus Resonance** (this project): MIT License - see `LICENSE` in the repository root.
- **Steam Audio** (Valve): Apache License 2.0 - [ValveSoftware/steam-audio](https://github.com/ValveSoftware/steam-audio). Bundled with the GDExtension; used for physics-based audio.
- **godot-cpp**: MIT License - used to build the GDExtension.

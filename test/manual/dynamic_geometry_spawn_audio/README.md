# Dynamic geometry spawn — spatial cutout regression

Verifies that spawning `ResonanceDynamicGeometry` while a `ResonancePlayer` is already playing does **not** hard-mute Nexus spatial output.

## What it does

1. Starts `ResonanceRuntime`, a listener, baseline dynamic geometry, and a looping ~440 Hz tone.
2. Waits until `ResonanceServer.is_spatial_audio_output_ready()` is true (cold-start warmup).
3. After ~2.5 s, spawns a second mesh with `ResonanceDynamicGeometry`.
4. Observes ~1.5 s: spatial ready must stay true and the player must keep playing.
5. Prints `PASS` / `FAIL` and quits (exit code 0 / 1) when `auto_quit` is on.

## Run (manual listen)

1. Sync addon + tests into the local Godot project (`project/` is gitignored):

   ```bash
   bash .github/scripts/prepare_godot_ci_project.sh
   ```

2. Open `project/` in Godot 4.6, open `res://test/manual/dynamic_geometry_spawn_audio/main.tscn`, press Play.

You should hear a continuous tone; when the spawned block appears, the tone must **not** cut to silence.

## Run (headless)

`godot` is often not on PATH. Use your local binary, e.g.:

```powershell
& "C:\Godot\4.7.2-stable\Godot_v4.7.2-stable_win64_console.exe" --path project --headless res://test/manual/dynamic_geometry_spawn_audio/main.tscn
```

Exit code `0` = PASS, `1` = FAIL.

**Important:** Sync the built GDExtension into `project/` first (`addons/nexus_resonance/bin/windows/` from the repo SoT), or the project may load a stale DLL.

## Automated GUT twin

[`test/unit/test_dynamic_geometry_spawn_keeps_spatial_ready.gd`](../../unit/test_dynamic_geometry_spawn_keeps_spatial_ready.gd) asserts the same gate/player invariants (Project → Tools → GUT, directory `res://test/unit`).

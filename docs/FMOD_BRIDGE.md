# Nexus Resonance FMOD Bridge

Nexus Resonance can act as a bridge between Steam Audio and FMOD Studio when used together with [fmod-gdextension](https://github.com/utopia-rise/fmod-gdextension). Setup involves several moving parts (Godot add-on, FMOD integration, and the Steam Audio FMOD plugin); follow the steps below.  
  
This is still experimental!

## Overview

- **Nexus Resonance** provides Steam Audio (Phonon): Scene, Simulator, HRTF, Occlusion, Reflections, Pathing
- **fmod-gdextension** provides FMOD Studio API to Godot (Events, Banks, FmodEventEmitter3D)
- **Steam Audio FMOD Plugin** (phonon_fmod.dll) runs as DSP inside FMOD and uses Nexus Resonance's state for spatialization

## Setup

### 1. Install Addons

1. Install Nexus Resonance (this addon)
2. Install [fmod-gdextension](https://github.com/utopia-rise/fmod-gdextension) into `addons/fmod/`
3. Install FMOD Studio API (from [fmod.com](https://www.fmod.com/download))

### 2. Build Steam Audio FMOD Plugin

```bash
python scripts/build_steam_audio_fmod.py --fmod-root "C:/path/to/fmodstudioapi" --deploy
```

This produces `phonon_fmod.dll` (Windows) and deploys it to `addons/nexus_resonance/bin/fmod_plugin/windows-x64/`.

### 3. Place Plugin for FMOD

Copy `phonon_fmod.dll` to the FMOD plugin path. For fmod-gdextension projects, this is typically:

- `addons/fmod/lib/windows-x64/` (FMOD Studio 2.2+)
- or `addons/fmod/platforms/win/lib/x86_64/`

### 4. Enable Bridge in ResonanceRuntime

In your scene with ResonanceRuntime, enable **FMOD Bridge** in the inspector. The bridge will initialize after ResonanceServer and connect Steam Audio context to the FMOD plugin.

## Usage for 3D Events

For FMOD 3D events that use the Steam Audio Spatializer:

1. **In FMOD Studio**: Add "Steam Audio Spatializer" to the 3D event's DSP chain
2. **In Godot** (GDScript):

```gdscript
# When starting a 3D event
var srv = Engine.get_singleton("ResonanceServer")
var handle = srv.create_source_handle(emitter_node.global_position, 1.0)
var bridge = ResonanceFMODBridgeScript.new()
bridge.init_bridge()
var fmod_handle = bridge.add_fmod_source(handle)
# Pass fmod_handle to FMOD event's Simulation Outputs Handle parameter
# (via event_instance.setParameterByIndex or similar FMOD API)
```

1. **Each frame** while the event plays: `srv.update_source(handle, emitter_node.global_position, 1.0)`
2. **When event stops**: `bridge.remove_fmod_source(fmod_handle)` and `srv.destroy_source_handle(handle)`

## ResonanceFmodEventEmitter

Add a **ResonanceFmodEventEmitter** node (GDExtension, under **Node3D** in Create Node) as a child of `FmodEventEmitter3D`. It creates a Resonance source and registers it with the bridge when `auto_play` is on and `ResonanceRuntime.fmod_bridge_enabled` is set. Position is synced each frame via `ResonanceServer.update_source`.

You must still pass the returned FMOD handle to the event's Steam Audio Spatializer "Simulation Outputs Handle" parameter via FMOD API when available (not wired in the emitter yet).

## Cooperation with fmod-gdextension

fmod-gdextension does not yet expose event lifecycle callbacks. To automate handle sync, you can:

- **Option A**: Wrap FmodEventEmitter3D with a script that hooks into `_process` and FMOD event state
- **Option B**: Request upstream: [utopia-rise/fmod-gdextension](https://github.com/utopia-rise/fmod-gdextension) could add signals/callbacks for event start/stop
- **Option C**: Use the bridge for reverb only (listener-centric); 3D events use FMOD's built-in 3D or manual handle management

## API Summary


| Method                                                  | Description                                                             |
| ------------------------------------------------------- | ----------------------------------------------------------------------- |
| `ResonanceFMODBridge.init_bridge()`                     | Connect Steam Audio to FMOD plugin. Call after both are initialized.    |
| `ResonanceFMODBridge.shutdown_bridge()`                 | Disconnect and unload plugin.                                           |
| `ResonanceFMODBridge.add_fmod_source(resonance_handle)` | Register source for FMOD Spatializer. Returns handle for DSP parameter. |
| `ResonanceFMODBridge.remove_fmod_source(fmod_handle)`   | Unregister when event stops.                                            |



@tool
extends RefCounted
class_name ResonanceFMODBridgeScript

## GDScript wrapper for the ResonanceFMODBridge (C++).
## Use when combining Nexus Resonance with fmod-gdextension.
## Call init_bridge() after both ResonanceServer and FMOD are initialized.
##
## Setup:
## 1. Install fmod-gdextension and Nexus Resonance
## 2. Build Steam Audio FMOD plugin (scripts/build_steam_audio_fmod.py)
## 3. Place phonon_fmod.dll in FMOD plugin path (e.g. addons/fmod/lib/windows-x64/)
## 4. Create bridge instance and call init_bridge() from _ready() after server init

var _bridge: Object = null


## Initialize the FMOD bridge. Call after ResonanceServer and FMOD are ready.
## Returns true on success. Creates C++ bridge instance on first call (lazy init).
func init_bridge() -> bool:
	if (
		_bridge == null
		and ResonanceServerAccess.has_server()
		and ClassDB.class_exists("ResonanceFMODBridge")
	):
		_bridge = ClassDB.instantiate("ResonanceFMODBridge")
	if not _bridge:
		push_warning(
			"ResonanceFMODBridgeScript: ResonanceFMODBridge not available. Ensure Nexus Resonance GDExtension is loaded."
		)
		return false
	if _bridge.init_bridge():
		return true
	return false


## Shutdown the bridge. Call before exiting or when switching audio backend.
func shutdown_bridge() -> void:
	if _bridge:
		_bridge.shutdown_bridge()


## Rebind Steam Audio FMOD plugin to a new ResonanceServer context after engine reinit.
func rebind_after_reinit() -> bool:
	if (
		_bridge == null
		and ResonanceServerAccess.has_server()
		and ClassDB.class_exists("ResonanceFMODBridge")
	):
		_bridge = ClassDB.instantiate("ResonanceFMODBridge")
	if not _bridge:
		return false
	return _bridge.rebind_after_reinit()


## Whether the bridge is loaded and initialized.
func is_bridge_loaded() -> bool:
	return _bridge != null and _bridge.is_bridge_loaded()


## Add an FMOD 3D event source for Steam Audio spatialization.
## resonance_source_handle: from ResonanceServer.create_source_handle()
## Returns handle to pass to Steam Audio Spatializer DSP parameter (Simulation Outputs Handle).
func add_fmod_source(resonance_source_handle: int) -> int:
	if not _bridge:
		return -1
	return _bridge.add_fmod_source(resonance_source_handle)


## Remove an FMOD source when the event stops.
func remove_fmod_source(fmod_handle: int) -> void:
	if _bridge:
		_bridge.remove_fmod_source(fmod_handle)

@tool
@icon("res://addons/nexus_resonance/ui/icons/resonance_config.svg")
extends Resource
class_name ResonancePlayerConfig

## Per-source preset for ResonancePlayer (Resource). **Use Global** = follow runtime unless noted.

# --- Distance / Attenuation ---
@export_group("Distance")
## Distance (meters) at which sound is at full volume. Closer than this: no attenuation.
@export_range(0.1, 100.0, 0.1) var min_distance: float = 1.0
## Max distance (meters) for attenuation. Sound reaches minimum volume at this range.
@export_range(1.0, 2000.0, 1.0) var max_distance: float = 500.0
var _attenuation_mode: int = 0
## Inverse / Linear / Curve / Disabled. Disabled turns off sim distance attenuation on the direct path (not mute). Legacy tres may map old flags into this enum.
@export_enum("Inverse:0", "Linear:1", "Curve:2", "Disabled:3") var attenuation_mode: int:
	get:
		return _attenuation_mode
	set(v):
		if _attenuation_mode != v:
			_attenuation_mode = v
			notify_property_list_changed()
## Custom attenuation curve. X = normalized distance (0..1), Y = volume. Used when attenuation_mode is Curve.
@export var attenuation_curve: Curve = null

# --- Direct Sound ---
@export_group("Direct Sound")
## Radius of the sound source in meters (Steam Audio occlusion radius). Affects volumetric occlusion sampling and diffraction; slightly larger radius can reduce edge flicker when [member ResonanceRuntimeConfig.occlusion_type] is Volumetric.
@export_range(0.1, 10.0, 0.1) var source_radius: float = 1.0
var _air_absorption_enabled: bool = true
## Enable distance-based air absorption. Distant sounds appear muffled.
@export var air_absorption_enabled: bool = true:
	get:
		return _air_absorption_enabled
	set(v):
		if _air_absorption_enabled != v:
			_air_absorption_enabled = v
			notify_property_list_changed()
var _air_absorption_input: int = 0
## Air absorption source: Simulation Defined = physics-based, User Defined = use low/mid/high sliders.
@export_enum("Simulation Defined:0", "User Defined:1") var air_absorption_input: int:
	get:
		return _air_absorption_input
	set(v):
		if _air_absorption_input != v:
			_air_absorption_input = v
			notify_property_list_changed()
## Low-band (≤800 Hz) EQ. 0 = fully attenuated, 1 = no change. Only when air_absorption_input is User Defined.
@export_range(0.0, 1.0, 0.01) var air_absorption_low: float = 1.0
## Mid-band (800 Hz–8 kHz) EQ. 0 = fully attenuated, 1 = no change.
@export_range(0.0, 1.0, 0.01) var air_absorption_mid: float = 1.0
## High-band (≥8 kHz) EQ. 0 = fully attenuated, 1 = no change.
@export_range(0.0, 1.0, 0.01) var air_absorption_high: float = 1.0

# --- Directivity ---
@export_group("Directivity")
var _directivity_enabled: bool = false
## If enabled, the sound source becomes directional. Projects along negative Z-axis (Forward).
@export var directivity_enabled: bool:
	get:
		return _directivity_enabled
	set(v):
		if _directivity_enabled != v:
			_directivity_enabled = v
			notify_property_list_changed()
var _directivity_input: int = 0
## Directivity source: Simulation Defined = dipole model (weight, power). User Defined = use directivity_value (script-controlled).
@export_enum("Simulation Defined:0", "User Defined:1") var directivity_input: int:
	get:
		return _directivity_input
	set(v):
		if _directivity_input != v:
			_directivity_input = v
			notify_property_list_changed()
## Shape: 0 = Omnidirectional, 1 = Dipole (figure-8). Intermediate = blend. Only when directivity_input is Simulation.
@export_range(0.0, 1.0, 0.01) var directivity_weight: float = 0.0
## Sharpness of the directivity pattern. 0 = broad cone, 4 = narrow beam. Only when directivity_input is Simulation.
@export_range(0.0, 4.0, 0.1) var directivity_power: float = 1.0
## Directivity attenuation (0-1). 0 = fully attenuated, 1 = no change. Only when directivity_input is User Defined.
@export_range(0.0, 1.0, 0.01) var directivity_value: float = 1.0

# --- Output ---
@export_group("Output")
var _bus_override: int = -1
## Direct + pathing bus. Use Global = runtime bus; Custom = [member bus_name]. Setter refreshes inspector visibility for [member bus_name].
@export_enum("Use Global:-1", "Custom:0") var bus_override: int = -1:
	get:
		return _bus_override
	set(v):
		if _bus_override != v:
			_bus_override = v
			notify_property_list_changed()
## Bus for Direct + Pathing when bus_override is Custom. Pick from existing buses in Audio Bus Layout.
@export var bus_name: StringName = &"Master"
var _reverb_bus_override: int = -1
## Reverb bus: Use Global or Custom ([member reverb_bus_name]). Parametric/Hybrid wet routing vs convolution mixer behavior follows runtime docs. Setter refreshes [member reverb_bus_name] in the inspector.
@export_enum("Use Global:-1", "Custom:0") var reverb_bus_override: int = -1:
	get:
		return _reverb_bus_override
	set(v):
		if _reverb_bus_override != v:
			_reverb_bus_override = v
			notify_property_list_changed()

## Bus for reverb output when reverb_bus_override is Custom. Pick from existing buses in Audio Bus Layout.
@export var reverb_bus_name: StringName = &"ResonanceReverb"

# --- Performance ---
@export_group("Performance")
## Minimum seconds between full playback-parameter updates (occlusion/reverb readback → [code]ResonanceInternalPlayback[/code]). 0 = every frame. E.g. 0.033 ≈ 30 Hz cap. Source simulation updates still run every frame (or batched).
@export_range(0.0, 0.5, 0.005) var playback_parameter_min_interval: float = 0.0
## Minimum source movement (meters) to trigger a full playback-parameter update when [member playback_parameter_min_interval] is also used; either condition can trigger. 0 = ignore movement-only gating (use interval only if set).
@export_range(0.0, 50.0, 0.05) var playback_parameter_min_move: float = 0.0
## Exponential smoothing time constant (seconds) for simulation-derived occlusion and transmission coefficients. 0 = off (instant). When greater than 0, playback parameters are pushed every frame while smoothing applies (higher CPU than [member playback_parameter_min_interval] alone). Only affects Simulation Defined occlusion/transmission, not User Defined.
@export_range(0.0, 0.5, 0.005) var playback_coeff_smoothing_time: float = 0.0

# --- Occlusion ---
@export_group("Occlusion")
## When off, occlusion is not simulated for this source; use User Defined [member occlusion_input] for manual occlusion.
@export var simulation_occlusion_enabled: bool = true
var _occlusion_input: int = 0
## Occlusion source: Simulation Defined = physics-based raycast. User Defined = use occlusion_value (script-controlled).
@export_enum("Simulation Defined:0", "User Defined:1") var occlusion_input: int:
	get:
		return _occlusion_input
	set(v):
		if _occlusion_input != v:
			_occlusion_input = v
			notify_property_list_changed()
## Occlusion attenuation (0-1). 0 = fully occluded, 1 = not occluded. Only when occlusion_input is User Defined.
@export_range(0.0, 1.0, 0.01) var occlusion_value: float = 1.0
var _occlusion_type_override: int = 2
## Raycast / Volumetric / Use Global ([code]2[/code]). Legacy [code]-1[/code] migrates to 2. Default export [code]= 2[/code] so new resources do not silently become Raycast.
@export_enum("Use Global:2", "Raycast:0", "Volumetric:1") var occlusion_type_override: int = 2:
	get:
		return _occlusion_type_override
	set(v):
		var nv := v
		if nv == -1:
			nv = 2
		if nv != 0 and nv != 1 and nv != 2:
			nv = 2
		if _occlusion_type_override != nv:
			_occlusion_type_override = nv
			notify_property_list_changed()
## Number of rays per source for volumetric occlusion (1–64; Steam Audio [code]numOcclusionSamples[/code]). Editable only when [member occlusion_type_override] is **Volumetric**. Higher values stabilize the occlusion fraction near geometry boundaries; lower = less CPU.
@export_range(1, 64, 1) var occlusion_samples: int = 64

# --- Transmission ---
@export_group("Transmission")
## When off, transmission through geometry is not simulated; use User Defined [member transmission_input] for manual bands.
@export var simulation_transmission_enabled: bool = true
var _transmission_input: int = 0
## Transmission source: Simulation Defined = physics-based. User Defined = use transmission low/mid/high (script-controlled).
@export_enum("Simulation Defined:0", "User Defined:1") var transmission_input: int:
	get:
		return _transmission_input
	set(v):
		if _transmission_input != v:
			_transmission_input = v
			notify_property_list_changed()
## Low-band transmission (0-1). Only when transmission_input is User Defined.
@export_range(0.0, 1.0, 0.01) var transmission_low: float = 1.0
## Mid-band transmission (0-1). Only when transmission_input is User Defined.
@export_range(0.0, 1.0, 0.01) var transmission_mid: float = 1.0
## High-band transmission (0-1). Only when transmission_input is User Defined.
@export_range(0.0, 1.0, 0.01) var transmission_high: float = 1.0
## Overrides runtime transmission mode for the direct effect only. Frequency independent = single coefficient; frequency dependent = three bands (see simulator transmission type).
@export_enum("Use Global:-1", "Frequency Independent:0", "Frequency Dependent:1")
var transmission_type_override: int = -1
var _max_transmission_surfaces_override: int = 0
## Use Global vs cap [member max_transmission_surfaces]. Legacy [code]-1[/code] → Use Global.
@export_enum("Use Global:0", "User Defined:1") var max_transmission_surfaces_override: int = 0:
	get:
		return _max_transmission_surfaces_override
	set(v):
		var nv := v
		if nv == -1:
			nv = 0
		if nv != 0 and nv != 1:
			nv = 0
		if _max_transmission_surfaces_override != nv:
			_max_transmission_surfaces_override = nv
			notify_property_list_changed()
## Max surfaces along the transmission path from listener (1–256; Steam Audio [code]numTransmissionRays[/code]). Only when [member max_transmission_surfaces_override] is User Defined. Increase for deep stacks of walls along one ray; it does not blend two materials at a lateral edge.
@export_range(1, 256, 1) var max_transmission_surfaces: int = 16

# --- Reflections (per-source) ---
@export_group("Reflections")
var _reflections_type: int = -1
## Reflections simulation: [b]Use Global[/b] = runtime [member ResonanceRuntimeConfig.default_reflections_mode] (Baked or Realtime). [b]Realtime[/b] here = per-source ray tracing (requires runtime [member ResonanceRuntimeConfig.realtime_rays] &gt; 0). Baked Reverb / Static Source / Listener = probe data modes.
@export_enum(
	"Use Global:-1",
	"Realtime:0",
	"Baked Reverb:1",
	"Baked Static Source:2",
	"Baked Static Listener:3"
)
var reflections_type: int = -1:
	get:
		return _reflections_type
	set(v):
		if _reflections_type != v:
			_reflections_type = v
			notify_property_list_changed()
## When reflections_type is Baked Static Source: reference to the node whose position was baked as static source. Leave empty to use this player's position.
@export var current_baked_source: NodePath = NodePath()
## When reflections_type is Baked Static Listener: reference to the node (e.g. listener/camera) whose position was baked. Leave empty to use active listener.
@export var current_baked_listener: NodePath = NodePath()
## Enable reflections for this source. Use Global = follow runtime.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var reflections_enabled: int = -1
## Enable pathing for this source. Use Global = follow runtime pathing_enabled.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var pathing_enabled_override: int = -1
## Wet occlusion on baked REVERB: Use Global / Off / On (outdoor leak vs indoor beds). See docs/baked-reflections-and-outdoor-sources.md.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1")
var apply_occlusion_to_baked_reflections_override: int = -1
## Baked REVERB probe choice: listener- vs source-centric (Use Global / override). Realtime ray origin unchanged.
@export_enum("Use Global:-1", "Listener-centric:0", "Source-centric:1")
var reflections_sampling_mode_override: int = -1
var _reverb_transmission_amount_input: int = 0
## Reverb transmission amount: Use Global or User Defined ([member reverb_transmission_amount]). Needs wet occlusion damping active.
@export_enum("Use Global:0", "User Defined:1") var reverb_transmission_amount_input: int = 0:
	get:
		return _reverb_transmission_amount_input
	set(v):
		if _reverb_transmission_amount_input != v:
			_reverb_transmission_amount_input = v
			notify_property_list_changed()
## 0–1 damping on reverb wet when User Defined and wet occlusion applies.
@export_range(0.0, 1.0, 0.01) var reverb_transmission_amount: float = 1.0

# --- Pathing ---
@export_group("Pathing")
## Path validation: Use Global = [member ResonanceRuntimeConfig.path_validation_enabled]. Disabled / Enabled = force off or on for this source.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var path_validation_override: int = -1
## Find alternate paths when a baked path is occluded. Use Global = [member ResonanceRuntimeConfig.find_alternate_paths]. Only applies when path validation is effectively on. Very CPU-heavy.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var find_alternate_paths_override: int = -1

# --- Mix Levels ---
@export_group("Mix Levels")
## Scales [member direct_mix_level], [member reflections_mix_level], and [member pathing_mix_level] together.
## Use for overall source level when the stream has no reliable volume (e.g. AudioStreamSynchronized).
@export_range(0.0, 10.0, 0.01) var master_mix_level: float = 1.0
## Volume of the direct (line-of-sight) sound path. Range 0-10. 1.0 = nominal.
@export_range(0.0, 10.0, 0.01) var direct_mix_level: float = 1.0
## Volume of reflections and reverb. Range 0-10. 1.0 = nominal.
@export_range(0.0, 10.0, 0.01) var reflections_mix_level: float = 1.0
## Volume of pathing (multi-path propagation). Range 0-10. Requires baked pathing data.
@export_range(0.0, 10.0, 0.01) var pathing_mix_level: float = 1.0

# --- Hybrid Reverb ---
@export_group("Hybrid Reverb")
## Per-source EQ multiplier for low band. 1.0 = no change. Only when runtime reflection_type is Hybrid.
@export_range(0.0, 4.0, 0.1) var reflections_eq_low: float = 1.0
## Per-source EQ multiplier for mid band. 1.0 = no change.
@export_range(0.0, 4.0, 0.1) var reflections_eq_mid: float = 1.0
## Per-source EQ multiplier for high band. 1.0 = no change.
@export_range(0.0, 4.0, 0.1) var reflections_eq_high: float = 1.0
## Samples before parametric part starts. -1 = use simulation value.
@export var reflections_delay: int = -1

# --- Spatialization ---
@export_group("Spatialization")
## Per-source override for [member ResonanceRuntimeConfig.direct_binaural]. Use Global = runtime default; Disabled = panning on dry path; Enabled = force HRTF.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var direct_binaural_override: int = -1
## Per-source override for [member ResonanceRuntimeConfig.reverb_binaural] (HRTF on convolution / mixer Ambisonics decode - reflections wet path).
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var reverb_binaural_override: int = -1
## Per-source override for [member ResonanceRuntimeConfig.pathing_binaural]. Disabled saves CPU when pathing runs but stereo speaker panning is enough.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var pathing_binaural_override: int = -1
## Blends this node's output between 2D (0) and full 3D spatial audio (1). At 0 the sound is panned as stereo (no HRTF / room simulation on the dry path); at 1 Nexus Resonance drives full spatialization, occlusion, and bus routing like a normal 3D source. Values in between mix the two (useful for UI voices vs world-attached sources).
@export_range(0.0, 1.0, 0.01) var spatial_blend: float = 1.0
## Encode point source to Ambisonics before binaural (HOA path). For mixing into an HOA-style chain. When enabled, [member spatial_blend] crossfades standard [code]iplBinauralEffect[/code] output (same spatialBlend HRIR behavior as when encode is off) with HOA encode+binaural: 0 = binaural only, 1 = HOA only; values in between mix both. Usually leave disabled.
@export var use_ambisonics_encode: bool = false
## HRTF table lookup: nearest (faster) vs bilinear (smoother motion). Use Global = [member ResonanceRuntimeConfig.hrtf_interpolation_bilinear].
@export_enum("Use Global:-1", "Nearest:0", "Bilinear:1") var hrtf_interpolation_override: int = -1
var _perspective_correction_override: int = -1
## Per-source perspective correction. Use Global = follow RuntimeConfig. Disabled = off. Enabled = force on for this source.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1")
var perspective_correction_override: int = -1:
	get:
		return _perspective_correction_override
	set(v):
		if _perspective_correction_override != v:
			_perspective_correction_override = v
			notify_property_list_changed()
## Factor for on-screen position mapping (0.5–2.0). 1.0 = calibrated for 30–32 inch monitor. Used when Enabled; ignored when Use Global.
@export_range(0.5, 2.0, 0.1) var perspective_factor: float = 1.0


func _validate_property(property: Dictionary) -> void:
	if property.name == "bus_name":
		if bus_override == -1:  # Use Global
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "reverb_bus_name":
		if reverb_bus_override == -1:  # Use Global
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "perspective_factor":
		if perspective_correction_override == 0:  # Disabled
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name in ["air_absorption_low", "air_absorption_mid", "air_absorption_high"]:
		if not air_absorption_enabled or air_absorption_input != 1:  # User Defined
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "attenuation_curve":
		if attenuation_mode != 2:  # Curve
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "current_baked_source":
		if reflections_type != 2:  # Baked Static Source
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "current_baked_listener":
		if reflections_type != 3:  # Baked Static Listener
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "occlusion_value":
		if occlusion_input != 1:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name in ["transmission_low", "transmission_mid", "transmission_high"]:
		if transmission_input != 1:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "reverb_transmission_amount":
		if reverb_transmission_amount_input != 1:  # not User Defined
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "occlusion_samples":
		if occlusion_type_override != 1:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "max_transmission_surfaces":
		if max_transmission_surfaces_override != 1:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name in ["directivity_weight", "directivity_power"]:
		if not directivity_enabled or directivity_input != 0:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "directivity_value":
		if not directivity_enabled or directivity_input != 1:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY


## Resolved direct/path bus: Use Global → [param global_fallback], else [member bus_name] if set.
func get_bus_name_effective(global_fallback: StringName) -> StringName:
	if bus_override == -1:  # Use Global
		return global_fallback
	var custom := bus_name
	return custom if not str(custom).is_empty() else global_fallback


## Resolved reverb bus: Use Global → [param global_fallback], else [member reverb_bus_name] if set.
func get_reverb_bus_name_effective(global_fallback: StringName) -> StringName:
	if reverb_bus_override == -1:  # Use Global
		return global_fallback
	var custom := reverb_bus_name
	return custom if not str(custom).is_empty() else global_fallback


## Creates default player config for sources without one assigned.
static func create_default() -> ResonancePlayerConfig:
	var cfg := ResonancePlayerConfig.new()
	cfg.occlusion_type_override = 2
	cfg.max_transmission_surfaces_override = 0
	return cfg

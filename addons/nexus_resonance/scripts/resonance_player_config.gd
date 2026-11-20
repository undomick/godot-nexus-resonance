@tool
@icon("res://addons/nexus_resonance/ui/icons/resonance_config.svg")
extends Resource
class_name ResonancePlayerConfig

## Per-source preset for [ResonancePlayer]. [b]Use Global[/b] follows the [ResonanceRuntime] setting.

const ResonanceConfigConstants = preload(
	"res://addons/nexus_resonance/scripts/resonance_config_constants.gd"
)

const REFLECTIONS_REALTIME := 0
const REFLECTIONS_STATIC_SOURCE := 2
const REFLECTIONS_STATIC_LISTENER := 3

@export_group("Distance")
var _distance_attenuation: bool = true
## Master distance attenuation switch. Off = full direct gain; mode and distances stay stored.
@export var distance_attenuation: bool = true:
	get:
		return _distance_attenuation
	set(v):
		if _distance_attenuation != v:
			_distance_attenuation = v
			notify_property_list_changed()
var _attenuation_mode: int = 0
## Inverse = physics 1/d (uses min_distance, ignores max). Linear / Curve = min/max rolloff. Disabled = no Direct rolloff.
@export_enum("Inverse:0", "Linear:1", "Curve:2", "Disabled:3") var attenuation_mode: int:
	get:
		return _attenuation_mode
	set(v):
		if _attenuation_mode != v:
			_attenuation_mode = v
			notify_property_list_changed()
## Distance in meters up to which the sound plays at full volume. Used by Inverse, Linear and Curve.
@export_range(0.1, 100.0, 0.1) var min_distance: float = 1.0
## Max distance (meters) for Linear/Curve rolloff. Grayed out for Inverse and Disabled.
@export_range(1.0, 2000.0, 1.0) var max_distance: float = 500.0
## Custom attenuation curve. X = normalized distance (0..1), Y = volume. Used when attenuation_mode is Curve.
@export var attenuation_curve: Curve = null
var _use_distance_curve_for_reflections: bool = false
## For Linear/Curve: also apply the distance model to reflections. Pathing always applies it to the path length.
@export var use_distance_curve_for_reflections: bool = false:
	get:
		return _use_distance_curve_for_reflections
	set(v):
		if _use_distance_curve_for_reflections != v:
			_use_distance_curve_for_reflections = v
			notify_property_list_changed()

@export_group("Direct Sound")
## Serialized occlusion radius. Prefer [member occlusion_radius] in the Occlusion group; this stays for .tres compatibility.
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
## Low-band (<= 800 Hz) EQ. 0 = fully attenuated, 1 = no change. Only when air_absorption_input is User Defined.
@export_range(0.0, 1.0, 0.01) var air_absorption_low: float = 1.0
## Mid-band (800 Hz - 8 kHz) EQ. 0 = fully attenuated, 1 = no change.
@export_range(0.0, 1.0, 0.01) var air_absorption_mid: float = 1.0
## High-band (>= 8 kHz) EQ. 0 = fully attenuated, 1 = no change.
@export_range(0.0, 1.0, 0.01) var air_absorption_high: float = 1.0

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

@export_group("Output")
var _bus_override: int = -1
## Direct + pathing bus. Use Global = runtime bus; Custom = [member bus_name].
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
## Bus this source sends to for the listener reverb: Use Global (runtime reverb bus) or Custom ([member reverb_bus_name]). A custom bus needs a [ResonanceAudioEffect] to apply the reverb.
@export_enum("Use Global:-1", "Custom:0") var reverb_bus_override: int = -1:
	get:
		return _reverb_bus_override
	set(v):
		if _reverb_bus_override != v:
			_reverb_bus_override = v
			notify_property_list_changed()

## Bus for the reverb send when reverb_bus_override is Custom. Pick from existing buses in Audio Bus Layout.
@export var reverb_bus_name: StringName = ResonancePaths.DEFAULT_REVERB_BUS_NAME

@export_group("Performance")
## Minimum seconds between full playback parameter updates (occlusion and reverb readback). 0 = every frame, 0.033 = about 30 Hz. Source simulation updates are not throttled.
@export_range(0.0, 0.5, 0.005) var playback_parameter_min_interval: float = 0.0
## Source movement in meters that triggers a full playback parameter update; either this or [member playback_parameter_min_interval] can trigger one. 0 = ignore movement.
@export_range(0.0, 50.0, 0.05) var playback_parameter_min_move: float = 0.0
## Smoothing time constant in seconds for simulated occlusion and transmission. 0 = off. Above 0, playback parameters are pushed every frame while smoothing, which costs more CPU. User Defined values are not smoothed.
@export_range(0.0, 0.5, 0.005) var playback_coeff_smoothing_time: float = 0.0

@export_group("Occlusion")
## Occlusion / volumetric sampling radius in meters. Same storage as [member source_radius].
@export_range(0.1, 10.0, 0.1) var occlusion_radius: float:
	get:
		return source_radius
	set(v):
		source_radius = v
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
## Raycast or Volumetric occlusion for this source, or Use Global. A legacy [code]-1[/code] reads as Use Global.
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
## Rays per source for volumetric occlusion (Steam Audio [code]numOcclusionSamples[/code]). Only when [member occlusion_type_override] is [b]Volumetric[/b]. More rays steady the occlusion near geometry edges; fewer cost less CPU.
@export_range(1, 64, 1) var occlusion_samples: int = 64

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
var _transmission_type_override: int = -1
## Overrides the runtime transmission mode for the direct effect only. Frequency Independent = one coefficient; Frequency Dependent = three bands.
@export_enum("Use Global:-1", "Frequency Independent:0", "Frequency Dependent:1")
var transmission_type_override: int = -1:
	get:
		return _transmission_type_override
	set(v):
		if _transmission_type_override != v:
			_transmission_type_override = v
			notify_property_list_changed()
var _max_transmission_surfaces_override: int = 0
## Use Global, or cap at [member max_transmission_surfaces]. A legacy [code]-1[/code] reads as Use Global.
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
## Max surfaces along the transmission path from the listener (Steam Audio [code]numTransmissionRays[/code]).
## [code]1[/code] = nearest surface only. Higher values multiply each hit and can silence transmission quickly.
## Only when [member max_transmission_surfaces_override] is User Defined. Does not blend materials at a lateral edge.
@export_range(1, 256, 1) var max_transmission_surfaces: int = 1

@export_group("Reflections")
## Send this source to the listener reverb ([member ResonanceListener.apply_reverb]) on its reverb bus.
@export var reverb_send: bool = true
## Per-source reflections on top of the listener reverb. Off by default; each source with reflections costs its own simulation and convolution.
@export var reflections_enabled: bool = false
## [b]Realtime[/b] = per-source ray tracing (requires runtime [member ResonanceRuntimeConfig.realtime_rays] &gt; 0). [b]Static Source[/b] uses this player's pose, [b]Static Listener[/b] the active listener; both need baked probe data (bake lists on [ResonanceProbeVolume]).
@export_enum("Realtime:0", "Static Source:2", "Static Listener:3")
var reflections_type: int = REFLECTIONS_REALTIME:
	set(nv):
		reflections_type = normalize_reflections_type(nv)

@export_group("Pathing")
## Enable pathing for this source. Use Global = follow runtime pathing_enabled.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var pathing_enabled_override: int = -1
## Path validation: Use Global = [member ResonanceRuntimeConfig.path_validation_enabled]. Disabled / Enabled = force off or on for this source.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var path_validation_override: int = -1
## Find alternate paths when a baked path is occluded. Use Global = [member ResonanceRuntimeConfig.find_alternate_paths]. Only applies when path validation is effectively on. Very CPU-heavy.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var find_alternate_paths_override: int = -1

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

@export_group("Hybrid Reverb")
## Per-source EQ multiplier for low band. 1.0 = no change. Only when runtime reflection_type is Hybrid.
@export_range(0.0, 4.0, 0.1) var reflections_eq_low: float = 1.0
## Per-source EQ multiplier for mid band. 1.0 = no change.
@export_range(0.0, 4.0, 0.1) var reflections_eq_mid: float = 1.0
## Per-source EQ multiplier for high band. 1.0 = no change.
@export_range(0.0, 4.0, 0.1) var reflections_eq_high: float = 1.0
## Samples before parametric part starts. -1 = use simulation value.
@export var reflections_delay: int = -1

@export_group("Spatialization")
## Per-source override for [member ResonanceRuntimeConfig.direct_binaural]. Use Global = runtime default; Disabled = panning on dry path; Enabled = force HRTF.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var direct_binaural_override: int = -1
## Per-source HRTF for decoding this player's own reflections (Parametric/Hybrid); pure Parametric output is mono and gets no HRTF.
## The Convolution/TAN reverb bus always follows [member ResonanceRuntimeConfig.reverb_binaural].
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var reverb_binaural_override: int = -1
## Per-source override for [member ResonanceRuntimeConfig.pathing_binaural]. Disabled saves CPU when pathing runs but stereo speaker panning is enough.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var pathing_binaural_override: int = -1
## Blends between 2D (0) and full 3D audio (1). At 0 the dry path is plain stereo panning without HRTF or room simulation;
## at 1 the source gets full spatialization and occlusion. Values in between mix both, e.g. for UI voices.
@export_range(0.0, 1.0, 0.01) var spatial_blend: float = 1.0
## Encodes the source to Ambisonics before binaural rendering. [member spatial_blend] then crossfades plain binaural (0) with the Ambisonics path (1).
## Usually leave disabled.
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
## Factor for on-screen position mapping. 1.0 = calibrated for a 30-32 inch monitor. Used when Enabled; ignored when Use Global.
@export_range(0.5, 2.0, 0.1) var perspective_factor: float = 1.0


func _validate_property(property: Dictionary) -> void:
	if property.name == "bus_name":
		if bus_override == -1:  # Use Global
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "reverb_bus_name":
		if reverb_bus_override == -1:  # Use Global
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "perspective_factor":
		if perspective_correction_override != 1:  # Enabled
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name in ["air_absorption_low", "air_absorption_mid", "air_absorption_high"]:
		if not air_absorption_enabled or air_absorption_input != 1:  # User Defined
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "source_radius":
		# Script alias of occlusion_radius, hidden in the inspector.
		property["usage"] = PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_SCRIPT_VARIABLE
	elif property.name == "attenuation_mode":
		if not distance_attenuation:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "min_distance":
		if not distance_attenuation or attenuation_mode == 3:  # Disabled
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "max_distance":
		# Inverse and Disabled ignore max_distance.
		if not distance_attenuation or attenuation_mode == 0 or attenuation_mode == 3:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "attenuation_curve":
		if not distance_attenuation or attenuation_mode != 2:  # Curve
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "use_distance_curve_for_reflections":
		if not distance_attenuation or (attenuation_mode != 1 and attenuation_mode != 2):
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "occlusion_value":
		if occlusion_input != 1:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name in ["transmission_low", "transmission_mid", "transmission_high"]:
		if transmission_input != 1:
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


## Direct and pathing bus: [param global_fallback] for Use Global or an empty [member bus_name], else [member bus_name].
func get_bus_name_effective(global_fallback: StringName) -> StringName:
	if bus_override == -1:  # Use Global
		return global_fallback
	var custom := bus_name
	return custom if not str(custom).is_empty() else global_fallback


## Reverb bus: [param global_fallback] for Use Global or an empty [member reverb_bus_name], else [member reverb_bus_name].
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


## Maps the removed 1.1 values Use Global ([code]-1[/code]) and Baked Reverb ([code]1[/code]) to Realtime; the static values kept their numbers.
static func normalize_reflections_type(value: int) -> int:
	if value == REFLECTIONS_STATIC_SOURCE or value == REFLECTIONS_STATIC_LISTENER:
		return value
	return REFLECTIONS_REALTIME

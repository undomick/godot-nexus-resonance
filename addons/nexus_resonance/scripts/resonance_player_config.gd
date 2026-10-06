@tool
@icon("res://addons/nexus_resonance/ui/icons/resonance_config.svg")
extends Resource
class_name ResonancePlayerConfig

## Per-source preset for ResonancePlayer (Resource). **Use Global** = follow runtime unless noted.

const ResonanceConfigConstants = preload(
	"res://addons/nexus_resonance/scripts/resonance_config_constants.gd"
)

# --- Distance / Attenuation ---
@export_group("Distance")
var _distance_attenuation: bool = true
## Master distance-attenuation switch. Off = full Direct gain and no Phonon distance model; mode/min/max stay stored.
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
## Distance (meters) at which sound is at full volume. Closer than this: no attenuation. Used by Inverse (1/d knee) and Linear/Curve.
@export_range(0.1, 100.0, 0.1) var min_distance: float = 1.0
## Max distance (meters) for Linear/Curve rolloff. Grayed out for Inverse and Disabled.
@export_range(1.0, 2000.0, 1.0) var max_distance: float = 500.0
## Custom attenuation curve. X = normalized distance (0..1), Y = volume. Used when attenuation_mode is Curve.
@export var attenuation_curve: Curve = null
var _use_distance_curve_for_reflections: bool = false
## When Linear/Curve: feed the same Phonon callback model into Reflections IR correction. Default off. Pathing always uses the model for Linear/Curve on path length.
@export var use_distance_curve_for_reflections: bool = false:
	get:
		return _use_distance_curve_for_reflections
	set(v):
		if _use_distance_curve_for_reflections != v:
			_use_distance_curve_for_reflections = v
			notify_property_list_changed()

# --- Direct Sound ---
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
## Reverb bus: Use Global or Custom ([member reverb_bus_name]). Custom only changes the Parametric/Hybrid split wet bus; Convolution/TAN wet always uses the runtime reverb bus. Setter refreshes [member reverb_bus_name] in the inspector.
@export_enum("Use Global:-1", "Custom:0") var reverb_bus_override: int = -1:
	get:
		return _reverb_bus_override
	set(v):
		if _reverb_bus_override != v:
			_reverb_bus_override = v
			notify_property_list_changed()

## Bus for reverb output when reverb_bus_override is Custom. Pick from existing buses in Audio Bus Layout.
@export var reverb_bus_name: StringName = ResonancePaths.DEFAULT_REVERB_BUS_NAME

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
var _transmission_type_override: int = -1
@export_enum("Use Global:-1", "Frequency Independent:0", "Frequency Dependent:1")
var transmission_type_override: int = -1:
	get:
		return _transmission_type_override
	set(v):
		if _transmission_type_override != v:
			_transmission_type_override = v
			notify_property_list_changed()
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
## Max surfaces along the transmission path from the listener (1–256; Steam Audio [code]numTransmissionRays[/code]).
## [code]1[/code] = nearest surface only. Higher values multiply each hit and can silence transmission quickly.
## Only when [member max_transmission_surfaces_override] is User Defined. Does not blend materials at a lateral edge.
@export_range(1, 256, 1) var max_transmission_surfaces: int = 1

# --- Reflections (per-source) ---
@export_group("Reflections")
## Enable reflections simulation for this source. Default on. Still gated by runtime reverb output. Unlike pathing, there is no runtime reflections_enabled flag.
@export var reflections_enabled: bool = true
## Reflections simulation: [b]Use Global[/b] = runtime [member ResonanceRuntimeConfig.default_reflections_mode] (Baked or Realtime). [b]Realtime[/b] here = per-source ray tracing (requires runtime [member ResonanceRuntimeConfig.realtime_rays] &gt; 0). Baked Reverb / Static Source / Listener = probe data modes. Static Source uses this player's pose; Static Listener uses the active listener (bake lists on [ResonanceProbeVolume]).
@export_enum(
	"Use Global:-1",
	"Realtime:0",
	"Baked Reverb:1",
	"Static Source:2",
	"Static Listener:3"
)
var reflections_type: int = -1

# --- Pathing ---
@export_group("Pathing")
## Enable pathing for this source. Use Global = follow runtime pathing_enabled.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var pathing_enabled_override: int = -1
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
## Per-source HRTF for local HOA decode of this player's reflection wet (parametric/hybrid direct path). Conv/TAN bus send uses global [member ResonanceRuntimeConfig.reverb_binaural] only. Pure parametric wet is mono (no HRTF). Use Global follows global reverb_binaural for local HOA decode.
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
		if perspective_correction_override != 1:  # Only editable when Enabled
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name in ["air_absorption_low", "air_absorption_mid", "air_absorption_high"]:
		if not air_absorption_enabled or air_absorption_input != 1:  # User Defined
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "source_radius":
		# Storage + script alias; inspector shows occlusion_radius.
		property["usage"] = PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_SCRIPT_VARIABLE
	elif property.name == "attenuation_mode":
		if not distance_attenuation:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "min_distance":
		if not distance_attenuation or attenuation_mode == 3:  # Disabled
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "max_distance":
		# Inverse and Disabled ignore max; gray out so it is self-explanatory.
		if not distance_attenuation or attenuation_mode == 0 or attenuation_mode == 3:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "attenuation_curve":
		if not distance_attenuation or attenuation_mode != 2:  # Curve
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "use_distance_curve_for_reflections":
		# Editable only with distance attenuation and Linear/Curve.
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


## Editor: warn when Custom reverb bus has no effect under Convolution/TAN reflection types.
func get_editor_reverb_bus_override_warning(owner: Node) -> String:
	if reverb_bus_override == -1:
		return ""
	var refl := _guess_reflection_type_for_owner(owner)
	if (
		refl == ResonanceConfigConstants.REFLECTION_TYPE_CONVOLUTION
		or refl == ResonanceConfigConstants.REFLECTION_TYPE_TAN
	):
		return (
			"Custom reverb_bus_override only affects Parametric/Hybrid split routing. "
			+ "Convolution/TAN wet stays on the runtime reverb bus."
		)
	return ""


func _guess_reflection_type_for_owner(owner: Node) -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv != null and srv.has_method("get_reflection_type"):
		return int(srv.get_reflection_type())
	if owner != null and owner.is_inside_tree():
		var tree: SceneTree = owner.get_tree()
		var root: Node = tree.edited_scene_root if tree else null
		if root == null and tree:
			root = tree.current_scene
		if root == null:
			root = owner
		var rt := _find_resonance_runtime(root)
		if rt != null:
			var cfg: Variant = rt.get("runtime")
			if cfg != null:
				return int(cfg.get("reflection_type"))
	return ResonanceConfigConstants.REFLECTION_TYPE_CONVOLUTION


static func _find_resonance_runtime(node: Node) -> Node:
	if not node:
		return null
	if node.is_class("ResonanceRuntime"):
		return node
	for c in node.get_children():
		var found := _find_resonance_runtime(c)
		if found:
			return found
	return null


## Creates default player config for sources without one assigned.
static func create_default() -> ResonancePlayerConfig:
	var cfg := ResonancePlayerConfig.new()
	cfg.occlusion_type_override = 2
	cfg.max_transmission_surfaces_override = 0
	return cfg

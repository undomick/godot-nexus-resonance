@tool
@icon("res://addons/nexus_resonance/ui/icons/resonance_config.svg")
extends Resource
class_name ResonanceBakeConfig

## Per-volume bake configuration. Link from ResonanceProbeVolume for presets.
## Saves as .tres for reusable presets. Falls back to create_default() when null.

const Constants = preload("resonance_config_constants.gd")

# --- Reflection (API kept; inspector-hidden - bake uses ResonanceRuntime.reflection_type) ---
## Legacy API / .tres field. Bake gating uses [ResonanceRuntime] reflection_type (TAN maps to Convolution).
@export_enum("Convolution:0", "Parametric:1", "Hybrid:2")
var reflection_type: int = Constants.REFLECTION_TYPE_HYBRID

# --- Pathing (API kept; inspector-hidden - bake uses ResonanceRuntime.pathing_enabled) ---
var _pathing_enabled: bool = false
## Legacy API / .tres field. Pathing bake gating uses [ResonanceRuntime] pathing_enabled.
## Pathing quality (vis range, path range, radius, threshold, samples) lives on [ResonanceRuntimeConfig].
@export var pathing_enabled: bool:
	get:
		return _pathing_enabled
	set(v):
		if _pathing_enabled != v:
			_pathing_enabled = v
			notify_property_list_changed()

# --- Additional Bake (API kept; inspector-hidden - gated by bake_sources / bake_listeners) ---
## Legacy flag kept for .tres / script API. Bake gating uses [member ResonanceProbeVolume.bake_sources].
@export var static_source_enabled: bool = false
## Legacy flag kept for .tres / script API. Bake gating uses [member ResonanceProbeVolume.bake_listeners].
@export var static_listener_enabled: bool = false

# --- Quality ---
@export_group("Quality")
## Bake Ambisonic Order for this volume's probe IRs. Use Global follows [ResonanceRuntimeConfig.bake_ambisonic_order].
@export_enum("Use Global:0", "1st Order:1", "2nd Order:2", "3rd Order:3")
var bake_ambisonics_order: int = 0
## Reflection rays per probe. Higher = better quality, longer bake.
@export_range(256, 16384, 256) var bake_num_rays: int = 4096
## Reflection bounces per ray. Higher = longer reverb tail, longer bake.
@export_range(1, 32, 1) var bake_num_bounces: int = 4
## Parallel bake threads. More = faster bake, more CPU.
@export_range(1, 64, 1) var bake_num_threads: int = 2


func _validate_property(property: Dictionary) -> void:
	if (
		property.name
		in [
			"static_source_enabled",
			"static_listener_enabled",
			"reflection_type",
			"pathing_enabled",
		]
	):
		property["usage"] = PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_SCRIPT_VARIABLE
		return


## Returns bake params dictionary for C++ set_bake_params.
## Pathing vis/path range, radius, and threshold come from [ResonanceRuntimeConfig] via bake_params_from_runtime.
func get_bake_params() -> Dictionary:
	return {
		"bake_ambisonics_order": bake_ambisonics_order,
		"bake_num_rays": bake_num_rays,
		"bake_num_bounces": bake_num_bounces,
		"bake_num_threads": bake_num_threads,
		"bake_reflection_type": reflection_type,
	}


## Creates default bake config for volumes without one assigned.
static func create_default() -> ResonanceBakeConfig:
	return ResonanceBakeConfig.new()

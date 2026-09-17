@tool
extends RefCounted

## Shared enums and hints for Resonance configs. Single source of truth.
## Reduces DRY violations between BakeConfig, RuntimeConfig, and PlayerConfig.

# Numeric values for reflection_type (avoid magic numbers).
const REFLECTION_TYPE_CONVOLUTION := 0
const REFLECTION_TYPE_PARAMETRIC := 1
const REFLECTION_TYPE_HYBRID := 2
const REFLECTION_TYPE_TAN := 3

## Bake layer stored on ResonanceProbeData (0-2). TAN maps to Convolution.
static func bake_reflection_type_from_runtime(runtime_type: int) -> int:
	if runtime_type == REFLECTION_TYPE_PARAMETRIC:
		return REFLECTION_TYPE_PARAMETRIC
	if runtime_type == REFLECTION_TYPE_HYBRID:
		return REFLECTION_TYPE_HYBRID
	return REFLECTION_TYPE_CONVOLUTION


## Exact match for bake needs / editor checks. Legacy baked_type -1 is always OK.
static func baked_reflection_type_matches_runtime(baked_type: int, runtime_type: int) -> bool:
	if baked_type < 0:
		return true
	return baked_type == bake_reflection_type_from_runtime(runtime_type)


# Display names for debug overlay and tooltips. BakeConfig only supports 0/1/2 (no TAN).
const REFLECTION_DISPLAY_NAMES: Array[String] = [
	"Convolution", "Parametric", "Hybrid", "TrueAudio Next"
]

# Export enum hints. Bake has no TAN (TrueAudio Next).
const REFLECTION_CONVOLUTION := "Convolution:0"
const REFLECTION_PARAMETRIC := "Parametric:1"
const REFLECTION_HYBRID := "Hybrid:2"
const REFLECTION_TAN := "TrueAudio Next (AMD GPU):3"

## Runtime config hot-reload policy (SSOT lives in C++: resonance_runtime_config_policy.cpp).
static func runtime_config_property_requires_engine_reinit(property: StringName) -> bool:
	if ClassDB.class_exists("ResonanceServer"):
		return ResonanceServer.runtime_config_property_requires_engine_reinit(property)
	return false

@tool
extends RefCounted
class_name ResonanceConfigConstants

## Shared enums and hints for Resonance configs. Single source of truth.
## Reduces DRY violations between BakeConfig, RuntimeConfig, and PlayerConfig.

# Numeric values for reflection_type (avoid magic numbers).
const REFLECTION_TYPE_CONVOLUTION := 0
const REFLECTION_TYPE_PARAMETRIC := 1
const REFLECTION_TYPE_HYBRID := 2
const REFLECTION_TYPE_TAN := 3

# Display names for debug overlay and tooltips. BakeConfig only supports 0/1/2 (no TAN).
const REFLECTION_DISPLAY_NAMES: Array[String] = [
	"Convolution", "Parametric", "Hybrid", "TrueAudio Next"
]

# Export enum hints. Bake has no TAN (TrueAudio Next).
const REFLECTION_CONVOLUTION := "Convolution:0"
const REFLECTION_PARAMETRIC := "Parametric:1"
const REFLECTION_HYBRID := "Hybrid:2"
const REFLECTION_TAN := "TrueAudio Next (AMD GPU):3"

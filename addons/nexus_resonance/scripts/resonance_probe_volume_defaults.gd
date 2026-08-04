extends Object
class_name ResonanceProbeVolumeDefaults

## Shared editor defaults for ResonanceProbeVolume (probe_data + bake_config).
## Used by the inspector when _ready / ENTER_TREE is not enough at node creation time.

const ResonanceBakeConfig = preload("res://addons/nexus_resonance/scripts/resonance_bake_config.gd")


## Ensures [param vol] has probe_data and bake_config. Returns true if anything was assigned.
static func ensure_resources(vol: Object) -> bool:
	if vol == null or not vol.is_class("ResonanceProbeVolume"):
		return false
	var changed := false
	if vol.has_method("ensure_default_resources"):
		var before_pd: Variant = vol.get_probe_data() if vol.has_method("get_probe_data") else null
		var before_bc: Variant = (
			vol.get_bake_config() if vol.has_method("get_bake_config") else null
		)
		vol.ensure_default_resources()
		var after_pd: Variant = vol.get_probe_data() if vol.has_method("get_probe_data") else null
		var after_bc: Variant = vol.get_bake_config() if vol.has_method("get_bake_config") else null
		changed = before_pd != after_pd or before_bc != after_bc
		# GDScript fallback if C++ could not load the bake config script.
		if vol.has_method("get_bake_config") and vol.get_bake_config() == null:
			vol.set_bake_config(ResonanceBakeConfig.create_default())
			changed = true
		return changed

	if vol.has_method("get_probe_data") and vol.get_probe_data() == null:
		vol.set_probe_data(ClassDB.instantiate("ResonanceProbeData"))
		changed = true
	if vol.has_method("get_bake_config") and vol.get_bake_config() == null:
		vol.set_bake_config(ResonanceBakeConfig.create_default())
		changed = true
	return changed

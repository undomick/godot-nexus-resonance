extends RefCounted

## Pure helpers for ResonanceReverbDataPoint inspector (testable without EditorInterface).

const BANDS := 3
const BAND_LABELS := ["Low (400 Hz)", "Mid (2.5 kHz)", "High (15 kHz)"]

const ERR_MESSAGES := {
	"no_context":
	"ResonanceServer is not initialized. Click Sample again after ResonanceRuntime is in the scene.",
	"no_resonance_server": "ResonanceServer is missing. Enable the Nexus Resonance plugin.",
	"probe_data_missing":
	"No probe data. Assign Probe Data or place under a baked ResonanceProbeVolume.",
	"invalid_context_or_probe_data": "Probe data is empty or invalid. Bake the Probe Volume first.",
	"probe_batch_load_failed": "Could not load the baked probe batch.",
	"probe_index_out_of_range": "Probe index out of range.",
	"no_probes": "Probe data has no probes.",
	"no_neighbors_in_radius":
	"No baked probes within Neighbor Radius. Move the marker or raise Neighbor Radius.",
	"energy_field_create_failed": "Could not create an energy field for sampling.",
}


static func find_ancestor_probe_data(node: Node) -> Variant:
	if node == null:
		return null
	var parent: Node = node.get_parent()
	while parent != null:
		if parent.is_class("ResonanceProbeVolume"):
			return parent.get("probe_data")
		parent = parent.get_parent()
	return null


static func find_ancestor_probe_volume(node: Node) -> Node:
	if node == null:
		return null
	var parent: Node = node.get_parent()
	while parent != null:
		if parent.is_class("ResonanceProbeVolume"):
			return parent
		parent = parent.get_parent()
	return null


static func human_error(code: String) -> String:
	if ERR_MESSAGES.has(code):
		return str(ERR_MESSAGES[code])
	if code.is_empty():
		return "Sampling failed."
	return "Sampling failed (%s)." % code


static func energy_to_db_text(energy: float) -> String:
	if energy <= 0.0 or not is_finite(energy):
		return "none"
	return "%.1f dB" % (10.0 * log(energy) / log(10.0))


static func band_energies_from_field(ef: Dictionary) -> PackedFloat32Array:
	var out := PackedFloat32Array([0.0, 0.0, 0.0])
	var channels := int(ef.get("channels", 0))
	var bins := int(ef.get("bins", 0))
	var bands := int(ef.get("bands", BANDS))
	if bands <= 0:
		bands = BANDS
	var data: Variant = ef.get("data", null)
	if not (data is PackedFloat32Array) or channels <= 0 or bins <= 0:
		return out
	var arr: PackedFloat32Array = data
	# Layout: ch * bands * bins + band * bins + bin
	for ch in range(channels):
		for band in range(mini(bands, BANDS)):
			var base := ch * bands * bins + band * bins
			var sum := 0.0
			for b in range(bins):
				var idx := base + b
				if idx >= arr.size():
					break
				var v: float = arr[idx]
				if v > 0.0:
					sum += v
			out[band] = out[band] + sum
	return out


static func format_query_lines(query: Variant, mix_rate: float = 48000.0) -> PackedStringArray:
	var lines: PackedStringArray = PackedStringArray()
	if typeof(query) != TYPE_DICTIONARY or (query as Dictionary).is_empty():
		lines.append("Not sampled yet. Click Sample Reverb Here.")
		return lines

	var d: Dictionary = query
	if not bool(d.get("ok", false)):
		lines.append(human_error(str(d.get("error", ""))))
		return lines

	lines.append("Baked reverb at this point")

	var rt: Variant = d.get("parametric_reverb_times", null)
	var rt_arr := PackedFloat32Array([0.0, 0.0, 0.0])
	if rt is PackedFloat32Array and (rt as PackedFloat32Array).size() >= 3:
		rt_arr = rt
	elif typeof(rt) == TYPE_ARRAY and (rt as Array).size() >= 3:
		rt_arr = PackedFloat32Array([float(rt[0]), float(rt[1]), float(rt[2])])
	lines.append("RT60")
	for i in range(BANDS):
		lines.append("  %s: %.2f s" % [BAND_LABELS[i], rt_arr[i]])

	var total := float(d.get("total_energy", 0.0))
	var band_e := PackedFloat32Array([0.0, 0.0, 0.0])
	var ef: Variant = d.get("energy_field", {})
	if typeof(ef) == TYPE_DICTIONARY and not (ef as Dictionary).is_empty():
		band_e = band_energies_from_field(ef)
		if total <= 0.0:
			total = band_e[0] + band_e[1] + band_e[2]
	lines.append("Energy")
	lines.append("  Total: %s" % energy_to_db_text(total))
	for i in range(BANDS):
		lines.append("  %s: %s" % [BAND_LABELS[i], energy_to_db_text(band_e[i])])

	var ir: Variant = d.get("impulse_response", null)
	if typeof(ir) == TYPE_DICTIONARY and not (ir as Dictionary).is_empty():
		var ird: Dictionary = ir
		var samples := int(ird.get("num_samples", ird.get("samples", 0)))
		var rate := mix_rate if mix_rate > 0.0 else 48000.0
		var duration := float(samples) / rate if samples > 0 else 0.0
		var peak := 0.0
		var ir_data: Variant = ird.get("data", null)
		if ir_data is PackedFloat32Array:
			for v in ir_data as PackedFloat32Array:
				var a := absf(float(v))
				if a > peak:
					peak = a
		lines.append("Impulse response (preview)")
		lines.append("  Duration: %.2f s" % duration)
		lines.append("  Peak: %.4f" % peak)

	return lines


static func query_has_energy(query: Variant) -> bool:
	if typeof(query) != TYPE_DICTIONARY:
		return false
	var d: Dictionary = query
	if not bool(d.get("ok", false)):
		return false
	return float(d.get("total_energy", 0.0)) > 0.0

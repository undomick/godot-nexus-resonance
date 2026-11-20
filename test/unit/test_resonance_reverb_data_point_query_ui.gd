extends GutTest

## ResonanceReverbDataPoint query UI helpers (ancestor probe_data + designer format lines).

const QueryUI = preload(
	"res://addons/nexus_resonance/scripts/resonance_reverb_data_point_query_ui.gd"
)


func test_find_ancestor_probe_data_from_volume_child() -> void:
	var root := Node3D.new()
	add_child_autoqfree(root)
	var vol = ClassDB.instantiate("ResonanceProbeVolume") as Node
	root.add_child(vol)
	var pd = ClassDB.instantiate("ResonanceProbeData") as Resource
	vol.set("probe_data", pd)

	var point = ClassDB.instantiate("ResonanceReverbDataPoint") as Node
	vol.add_child(point)
	await wait_process_frames(1)

	var found = QueryUI.find_ancestor_probe_data(point)
	assert_eq(found, pd)
	assert_not_null(QueryUI.find_ancestor_probe_volume(point))


func test_find_ancestor_probe_data_null_without_volume() -> void:
	var root := Node3D.new()
	add_child_autoqfree(root)
	var point = ClassDB.instantiate("ResonanceReverbDataPoint") as Node
	root.add_child(point)
	await wait_process_frames(1)
	assert_null(QueryUI.find_ancestor_probe_data(point))
	assert_null(QueryUI.find_ancestor_probe_volume(point))


func test_format_query_lines_empty() -> void:
	var lines: PackedStringArray = QueryUI.format_query_lines({})
	assert_eq(lines.size(), 1)
	assert_true(lines[0].contains("Not sampled yet"))


func test_format_query_lines_human_error() -> void:
	var lines: PackedStringArray = QueryUI.format_query_lines({"ok": false, "error": "no_neighbors_in_radius"})
	assert_eq(lines.size(), 1)
	assert_true(lines[0].contains("Neighbor Radius"))
	assert_false(lines[0].contains("ok:"))


func test_energy_to_db_text() -> void:
	assert_eq(QueryUI.energy_to_db_text(0.0), "none")
	assert_eq(QueryUI.energy_to_db_text(-1.0), "none")
	assert_eq(QueryUI.energy_to_db_text(1.0), "0.0 dB")
	assert_eq(QueryUI.energy_to_db_text(100.0), "20.0 dB")


func test_band_energies_from_field() -> void:
	# 1 channel, 3 bands, 2 bins: [b0b0, b0b1, b1b0, b1b1, b2b0, b2b1]
	var data := PackedFloat32Array([1.0, 1.0, 2.0, 2.0, 0.5, 0.5])
	var ef := {"channels": 1, "bins": 2, "bands": 3, "data": data}
	var bands: PackedFloat32Array = QueryUI.band_energies_from_field(ef)
	assert_eq(bands.size(), 3)
	assert_eq(bands[0], 2.0)
	assert_eq(bands[1], 4.0)
	assert_eq(bands[2], 1.0)


func test_format_query_lines_success_rt60_and_energy() -> void:
	var rt := PackedFloat32Array([0.5, 1.0, 1.5])
	var data := PackedFloat32Array([1.0, 0.0, 0.0, 0.0, 0.0, 0.0])
	var ef := {"channels": 1, "bins": 2, "bands": 3, "data": data}
	var d := {
		"ok": true,
		"total_energy": 1.0,
		"parametric_reverb_times": rt,
		"energy_field": ef,
		"impulse_response": {"channels": 1, "num_samples": 48000, "data": PackedFloat32Array([0.25, -0.5])}
	}
	var lines: PackedStringArray = QueryUI.format_query_lines(d, 48000.0)
	var joined := "\n".join(lines)
	assert_true(joined.contains("Baked reverb at this point"))
	assert_true(joined.contains("RT60"))
	assert_true(joined.contains("Low (400 Hz): 0.50 s"))
	assert_true(joined.contains("Mid (2.5 kHz): 1.00 s"))
	assert_true(joined.contains("High (15 kHz): 1.50 s"))
	assert_true(joined.contains("Energy"))
	assert_true(joined.contains("Total: 0.0 dB"))
	assert_true(joined.contains("Impulse response (preview)"))
	assert_true(joined.contains("Duration: 1.00 s"))
	assert_true(joined.contains("Peak: 0.5000"))
	assert_false(joined.contains("nearest_probe_index"))
	assert_false(joined.contains("energy_field.data.size"))


func test_query_has_energy() -> void:
	assert_false(QueryUI.query_has_energy({}))
	assert_false(QueryUI.query_has_energy({"ok": false, "total_energy": 1.0}))
	assert_false(QueryUI.query_has_energy({"ok": true, "total_energy": 0.0}))
	assert_true(QueryUI.query_has_energy({"ok": true, "total_energy": 0.01}))

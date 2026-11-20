extends GutTest

## Convolution voice blocks are gated on the work Nexus adds around the Steam Audio apply.

const Report := preload("res://test/perf/perf_report.gd")

const OVERHEAD := "nexus_overhead_us_per_voice_block"


func test_overhead_is_block_minus_effect_apply() -> void:
	var targets := {
		"per_scenario": {"convolution_8": {OVERHEAD: {"target": 40.0, "limit": 80.0}}},
	}
	var row := _find_row(Report.rate(_convolution_8(330.0, 305.0), targets, {}), OVERHEAD)
	assert_false(row.is_empty(), "overhead must be rated")
	assert_almost_eq(float(row["value"]), 25.0, 0.001)
	assert_eq(row["status"], Report.PASS)


func test_scenario_without_effect_apply_has_no_overhead() -> void:
	var direct := {"id": "direct_8", "voices": 8, "audio_us_per_voice_block": 20.0}
	var baseline := {"id": "baseline_empty", "voices": 0}
	var results: Array[Dictionary] = [direct, baseline]
	var targets := {"metrics": {OVERHEAD: {"target": 40.0, "limit": 80.0}}}
	var rated := Report.rate(results, targets, {})
	assert_true(_find_row(rated, OVERHEAD).is_empty(), "no apply split -> nothing to rate")


func test_project_targets_gate_convolution_on_overhead_not_block() -> void:
	var targets: Dictionary = JSON.parse_string(
		FileAccess.get_file_as_string("res://test/perf/perf_targets.json")
	)
	var rated := Report.rate(_convolution_8(330.0, 305.0), targets, {})
	assert_eq(_find_row(rated, OVERHEAD).get("status", ""), Report.PASS)
	assert_true(
		_find_row(rated, "audio_us_per_voice_block").is_empty(),
		"Steam Audio apply dominates the block and is reported, not rated"
	)


func _convolution_8(block_us: float, apply_us: float) -> Array[Dictionary]:
	var result := {
		"id": "convolution_8",
		"voices": 8,
		"audio_us_per_voice_block": block_us,
		"audio_split_us_per_voice_block": [{"key": "reflection_effect_apply_us", "us": apply_us}],
	}
	var results: Array[Dictionary] = [result]
	return results


func _find_row(rated: Dictionary, metric: String) -> Dictionary:
	for row in rated["rows"]:
		if row["metric"] == metric:
			return row
	return {}

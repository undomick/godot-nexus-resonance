extends RefCounted

## Rates measured scenarios against perf_targets.json and writes perf_report.json / perf_report.md.

const PASS := "PASS"
const WARN := "WARN"
const FAIL := "FAIL"
const SKIP := "SKIP"


static func rate(
	results: Array[Dictionary], targets: Dictionary, context: Dictionary
) -> Dictionary:
	var by_id := {}
	for r in results:
		by_id[r["id"]] = r
	_add_main_cost_per_player(results, by_id.get("baseline_empty", {}))
	_add_nexus_overhead(results)
	var rows: Array[Dictionary] = []
	var skip := {}
	if context.get("audio_driver", "") == "Dummy":
		skip["late_mix"] = "Dummy driver mixes in bursts; callback gaps say nothing here"
	for r in results:
		rows.append_array(_rate_absolute(r, targets, skip))
		rows.append_array(_rate_compare(r, by_id, targets))
		rows.append_array(_rate_expect(r))
		if not bool(r.get("spatial_ready", true)):
			rows.append(
				_row(
					r["id"],
					"spatial_ready",
					0,
					null,
					null,
					FAIL,
					"spatial output never became ready"
				)
			)
		if (
			not bool(r.get("worker_thread_found", true))
			and context.get("thread_cpu_supported", false)
		):
			rows.append(
				_row(
					r["id"],
					"worker_thread_found",
					0,
					null,
					null,
					WARN,
					"worker thread not named; worker_cpu_pct is 0"
				)
			)
	return {"results": results, "rows": rows}


static func crashed_row(scenario: String) -> Dictionary:
	return _row(
		scenario,
		"completed",
		0,
		1,
		1,
		FAIL,
		"no result: the Godot process crashed or timed out, see perf_%s.log" % scenario
	)


# Main-thread CPU also holds the engine and this harness, so subtract the empty baseline.
static func _add_main_cost_per_player(results: Array[Dictionary], baseline: Dictionary) -> void:
	if baseline.is_empty():
		return
	for r in results:
		var players := int(r.get("players", 0))
		if players > 0:
			r["main_us_per_player"] = (
				(float(r["main_frame_cpu_us"]) - float(baseline["main_frame_cpu_us"])) / players
			)


static func _add_nexus_overhead(results: Array[Dictionary]) -> void:
	for r in results:
		var split := _split_by_key(r)
		if not split.has("reflection_effect_apply_us") or not r.has("audio_us_per_voice_block"):
			continue
		r["nexus_overhead_us_per_voice_block"] = (
			float(r["audio_us_per_voice_block"]) - split["reflection_effect_apply_us"]
		)


static func has_fail(rated: Dictionary) -> bool:
	for row in rated["rows"]:
		if row["status"] == FAIL:
			return true
	return false


static func print_summary(rated: Dictionary) -> void:
	print("")
	print(
		(
			"%-22s %-30s %12s %10s %10s  %s"
			% ["scenario", "metric", "value", "target", "limit", "status"]
		)
	)
	for row in rated["rows"]:
		if row["status"] == PASS:
			continue
		print(
			(
				"%-22s %-30s %12s %10s %10s  %s %s"
				% [
					row["scenario"],
					row["metric"],
					_fmt(row["value"]),
					_fmt(row["target"]),
					_fmt(row["limit"]),
					row["status"],
					row["note"]
				]
			)
		)
	var counts := {PASS: 0, WARN: 0, FAIL: 0, SKIP: 0}
	for row in rated["rows"]:
		counts[row["status"]] += 1
	print("perf: %d PASS, %d WARN, %d FAIL" % [counts[PASS], counts[WARN], counts[FAIL]])


static func write(rated: Dictionary, context: Dictionary, out_dir: String) -> void:
	DirAccess.make_dir_recursive_absolute(out_dir)
	var json := FileAccess.open(out_dir.path_join("perf_report.json"), FileAccess.WRITE)
	if json != null:
		json.store_string(
			JSON.stringify(
				{"context": context, "results": rated["results"], "rows": rated["rows"]}, "\t"
			)
		)
	var md := FileAccess.open(out_dir.path_join("perf_report.md"), FileAccess.WRITE)
	if md != null:
		md.store_string(_markdown(rated, context))
	print("perf: report written to %s" % ProjectSettings.globalize_path(out_dir))


static func _rate_absolute(
	r: Dictionary, targets: Dictionary, skip: Dictionary
) -> Array[Dictionary]:
	var rows: Array[Dictionary] = []
	var metrics: Dictionary = targets.get("metrics", {}).duplicate()
	metrics.merge(targets.get("per_scenario", {}).get(r["id"], {}), true)
	for metric in metrics:
		if not r.has(metric):
			continue
		var t: Dictionary = metrics[metric]
		if skip.has(metric):
			rows.append(
				_row(r["id"], metric, r[metric], t["target"], t["limit"], SKIP, skip[metric])
			)
			continue
		rows.append(
			_row(
				r["id"],
				metric,
				r[metric],
				t["target"],
				t["limit"],
				_status(float(r[metric]), t),
				t.get("basis", "")
			)
		)
	return rows


static func _rate_compare(
	r: Dictionary, by_id: Dictionary, targets: Dictionary
) -> Array[Dictionary]:
	var rows: Array[Dictionary] = []
	for c in r.get("compare", []):
		var ref: Dictionary = by_id.get(c["vs"], {})
		var metric: String = c["metric"]
		var key: String = c.get("max_ratio", c.get("max_delta", ""))
		var t: Dictionary = targets.get("compare", {}).get(key, {})
		var label := "%s vs %s" % [metric, c["vs"]]
		if ref.is_empty() or t.is_empty():
			rows.append(_row(r["id"], label, null, null, null, SKIP, "reference scenario not run"))
			continue
		var value := float(r.get(metric, 0.0))
		var base := float(ref.get(metric, 0.0))
		var derived := value - base
		if c.has("max_ratio"):
			derived = value / base if base > 0.0 else 0.0
		rows.append(
			_row(
				r["id"],
				label,
				derived,
				t["target"],
				t["limit"],
				_status(derived, t),
				t.get("basis", "")
			)
		)
	return rows


static func _rate_expect(r: Dictionary) -> Array[Dictionary]:
	var rows: Array[Dictionary] = []
	for e in r.get("expect", []):
		var value := float(r.get(e["metric"], 0.0))
		var ok := value <= float(e["max"])
		rows.append(
			_row(
				r["id"],
				e["metric"],
				value,
				e["max"],
				e["max"],
				PASS if ok else FAIL,
				e.get("why", "")
			)
		)
	return rows


static func _status(value: float, t: Dictionary) -> String:
	if value <= float(t["target"]):
		return PASS
	if value <= float(t["limit"]):
		return WARN
	return FAIL


static func _row(
	scenario: String,
	metric: String,
	value: Variant,
	target: Variant,
	limit: Variant,
	status: String,
	note: String
) -> Dictionary:
	return {
		"scenario": scenario,
		"metric": metric,
		"value": value,
		"target": target,
		"limit": limit,
		"status": status,
		"note": note
	}


# Negative values mark a measurement that was not available.
static func _known_or_null(v: Variant) -> Variant:
	if v == null or float(v) < 0.0:
		return null
	return v


static func _fmt(v: Variant) -> String:
	if v == null:
		return "-"
	if typeof(v) == TYPE_FLOAT:
		return "%.1f" % v
	return str(v)


const _KEY_COLUMNS := [
	"voices",
	"audio_us_per_voice_block",
	"nexus_overhead_us_per_voice_block",
	"audio_nexus_load_pct",
	"audio_block_max_us",
	"main_tick_us_p95",
	"main_frame_cpu_us",
	"main_us_per_player",
	"main_peak_us_max",
	"spawn_us_max",
	"spawn_voice_dsp_us_max",
	"spawn_reflection_effect_us_max",
	"spawn_source_us_max",
	"spawn_params_push_us_max",
	"stream_static_pack_us_max",
	"stream_probe_batch_us_max",
	"material_swap_us_max",
	"spatial_ready_ms",
	"reverb_bus_us_p95",
	"worker_cpu_pct",
	"other_threads_cpu_pct",
	"reflections_us_max",
	"reflections_age_ms_p50",
	"reflections_age_ms_max",
	"reflections_interval_ms_p50",
	"reflections_interval_ms_max",
	"direct_sim_us_p50",
	"scene_commit_us_max",
	"sync_fetch_us_p95",
	"source_flush_try_lock_fail_per_sec",
	"direct_passes_during_reflections_per_sec",
	"reflection_passes_per_sec",
	"reflection_passes_skipped_per_sec",
	"pathing_passes_skipped_per_sec",
	"rays_min",
	"output_underrun",
]


static func _markdown(rated: Dictionary, context: Dictionary) -> String:
	var lines := PackedStringArray()
	lines.append("# Nexus Resonance perf report")
	lines.append("")
	lines.append(
		(
			"Godot %s, %s (%d threads), mix rate %d Hz, %s"
			% [
				context.get("godot", ""),
				context.get("cpu", ""),
				context.get("cores", 0),
				int(context.get("mix_rate", 0)),
				context.get("date", "")
			]
		)
	)
	lines.append("")
	lines.append("## Findings (WARN / FAIL)")
	lines.append("")
	lines.append("| Scenario | Metric | Value | Target | Limit | Status | Basis |")
	lines.append("|---|---|---|---|---|---|---|")
	for row in rated["rows"]:
		if row["status"] == PASS:
			continue
		lines.append(
			(
				"| %s | %s | %s | %s | %s | %s | %s |"
				% [
					row["scenario"],
					row["metric"],
					_fmt(row["value"]),
					_fmt(row["target"]),
					_fmt(row["limit"]),
					row["status"],
					row["note"]
				]
			)
		)
	lines.append("")
	lines.append("## Measurements")
	lines.append("")
	lines.append("| Scenario | " + " | ".join(_KEY_COLUMNS) + " |")
	lines.append("|---" + "|---".repeat(_KEY_COLUMNS.size()) + "|")
	for r in rated["results"]:
		var cells := PackedStringArray([r["id"]])
		for k in _KEY_COLUMNS:
			cells.append(_fmt(r.get(k, null)))
		lines.append("| " + " | ".join(cells) + " |")
	lines.append_array(_split_table(rated["results"]))
	lines.append_array(_spike_table(rated["results"]))
	lines.append("")
	lines.append("## Questions")
	lines.append("")
	for r in rated["results"]:
		lines.append("- **%s**: %s" % [r["id"], r["question"]])
	return "\n".join(lines) + "\n"


# Per voice block, in us. "effect apply" is the shared Steam Audio apply of all reflection
# rounds, including those closed outside a voice block.
static func _split_table(results: Array) -> PackedStringArray:
	var lines := PackedStringArray()
	var columns := _split_columns(results)
	if columns.is_empty():
		return lines
	lines.append("")
	lines.append("## Voice block breakdown (us per voice block)")
	lines.append("")
	var headers := PackedStringArray(["Scenario", "block"])
	for key in columns:
		headers.append(_split_label(key))
	lines.append("| " + " | ".join(headers) + " |")
	lines.append("|---" + "|---".repeat(headers.size() - 1) + "|")
	for r in results:
		var split := _split_by_key(r)
		if split.is_empty() or int(r.get("voices", 0)) == 0:
			continue
		var cells := PackedStringArray([r["id"], _fmt(r.get("audio_us_per_voice_block", null))])
		for key in columns:
			cells.append(_fmt(split.get(key, null)))
		lines.append("| " + " | ".join(cells) + " |")
	return lines


# Where the worst blocks come from: a long lap in one stage points at that stage's work, involuntary
# context switches of the audio thread or host steal time at preemption. "apply max" is the longest
# single shared Steam Audio reflection apply. Percentiles are bucket edges, up to 25% high.
static func _spike_table(results: Array) -> PackedStringArray:
	var lines := PackedStringArray()
	var stages := _stage_max_columns(results)
	if stages.is_empty():
		return lines
	lines.append("")
	lines.append("## Worst blocks (us, longest single lap per stage)")
	lines.append("")
	var headers := PackedStringArray(
		["Scenario", "max", "p99", "p99.9", "ctx switches", "steal %", "apply max"]
	)
	for key in stages:
		headers.append(key.trim_prefix("max_stage_").trim_suffix("_us"))
	lines.append("| " + " | ".join(headers) + " |")
	lines.append("|---" + "|---".repeat(headers.size() - 1) + "|")
	for r in results:
		if int(r.get("voices", 0)) == 0:
			continue
		var cells := PackedStringArray(
			[
				r["id"],
				_fmt(r.get("audio_block_max_us", null)),
				_fmt(r.get("audio_block_p99_us", null)),
				_fmt(r.get("audio_block_p999_us", null)),
				_fmt(r.get("audio_thread_involuntary_switches", null)),
				_fmt(_known_or_null(r.get("host_steal_pct", null))),
				_fmt(r.get("reflection_effect_apply_max_us", null)),
			]
		)
		var stage_max := {}
		for entry in r.get("audio_stage_max_us", []):
			stage_max[entry["key"]] = float(entry["us"])
		for key in stages:
			cells.append(_fmt(stage_max.get(key, null)))
		lines.append("| " + " | ".join(cells) + " |")
	return lines


static func _stage_max_columns(results: Array) -> Array[String]:
	var columns: Array[String] = []
	for r in results:
		for entry in r.get("audio_stage_max_us", []):
			if not columns.has(entry["key"]):
				columns.append(entry["key"])
	return columns


static func _split_columns(results: Array) -> Array[String]:
	var columns: Array[String] = []
	for r in results:
		for entry in r.get("audio_split_us_per_voice_block", []):
			if not columns.has(entry["key"]):
				columns.append(entry["key"])
	return columns


static func _split_by_key(r: Dictionary) -> Dictionary:
	var split := {}
	for entry in r.get("audio_split_us_per_voice_block", []):
		split[entry["key"]] = float(entry["us"])
	return split


static func _split_label(key: String) -> String:
	if key == "reflection_effect_apply_us":
		return "effect apply"
	if key == "reflection_outside_block_us":
		return "refl. outside block"
	return key.trim_prefix("stage_").trim_suffix("_us")

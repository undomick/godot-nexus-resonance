extends SceneTree

## Performance check: runs the scenarios from perf_scenarios.gd in a headless Godot, measures main thread,
## simulation worker and audio thread, and rates the results against perf_targets.json.
## scripts/run_perf_check.sh starts one process per scenario, so a crash only loses that scenario.
##
## godot --headless --audio-driver Dummy --path project -s res://test/perf/perf_check.gd -- <mode> [options]
##   --list              print scenario ids
##   --run=ID            measure one scenario, write ID.json into --out
##   --report            rate all ID.json files in --out, write perf_report.json / perf_report.md
##   --seconds=N         measure window per scenario (default 8)
##   --out=DIR           result directory (default user://perf)
##   --strict            with --report: exit code 1 on any FAIL

const Scenarios := preload("res://test/perf/perf_scenarios.gd")
const SceneBuilder := preload("res://test/perf/perf_scene_builder.gd")
const ThreadCpu := preload("res://test/perf/perf_thread_cpu.gd")
const Stats := preload("res://test/perf/perf_stats.gd")
const Report := preload("res://test/perf/perf_report.gd")
const TARGETS_PATH := "res://test/perf/perf_targets.json"

const FRAME_RATE := 60
const READY_TIMEOUT_FRAMES := 600
const SETTLE_SECONDS := 1.5
const AUDIO_POLL_SECONDS := 0.5
const TEARDOWN_FRAMES := 20
const REFLECTION_SPLIT_KEYS := ["reflection_effect_apply_us", "reflection_outside_block_us"]

var _thread_cpu := ThreadCpu.new()


func _initialize() -> void:
	Engine.max_fps = FRAME_RATE
	_run.call_deferred()


func _run() -> void:
	var args := _parse_args()
	if args["list"]:
		for scenario in Scenarios.all():
			print(scenario["id"])
		quit(0)
		return
	if args["report"]:
		quit(_write_report(args))
		return
	var scenario := _find_scenario(args["run"])
	if scenario.is_empty():
		push_error("perf: unknown scenario '%s'" % args["run"])
		quit(2)
		return
	print("[perf] %s ..." % scenario["id"])
	var result := await _run_scenario(scenario, float(args["seconds"]))
	result["context"] = _environment_context()
	DirAccess.make_dir_recursive_absolute(args["out"])
	var f := FileAccess.open(args["out"].path_join(scenario["id"] + ".json"), FileAccess.WRITE)
	f.store_string(JSON.stringify(result, "\t"))
	f.close()
	quit(0)


# A scenario with a log but no result file crashed or hung; it is reported as FAIL.
func _write_report(args: Dictionary) -> int:
	var results: Array[Dictionary] = []
	var missing := PackedStringArray()
	var context := {}
	for scenario in Scenarios.all():
		var path: String = args["out"].path_join(scenario["id"] + ".json")
		if not FileAccess.file_exists(path):
			if FileAccess.file_exists(args["out"].path_join("perf_%s.log" % scenario["id"])):
				missing.append(scenario["id"])
			continue
		var r: Dictionary = JSON.parse_string(FileAccess.get_file_as_string(path))
		context = r.get("context", context)
		r.erase("context")
		results.append(r)
	var rated := Report.rate(results, _load_targets(), context)
	for id in missing:
		rated["rows"].append(Report.crashed_row(id))
	Report.print_summary(rated)
	Report.write(rated, context, args["out"])
	return 1 if args["strict"] and Report.has_fail(rated) else 0


func _find_scenario(id: String) -> Dictionary:
	for scenario in Scenarios.all():
		if scenario["id"] == id:
			return scenario
	return {}


func _run_scenario(scenario: Dictionary, seconds: float) -> Dictionary:
	var builder := SceneBuilder.new()
	builder.build(scenario, root)
	await _await_server_started()
	await builder.bake_probes()
	builder.start_playback()
	var ready_ms := await _await_spatial_ready(builder, scenario)
	await _advance(builder, SETTLE_SECONDS)
	_drain_audio_counters(builder)

	var m := await _measure(builder, seconds)
	m["id"] = scenario["id"]
	m["question"] = scenario["question"]
	m["spatial_ready"] = ready_ms >= 0.0
	m["spatial_ready_ms"] = ready_ms
	m["churn_spawned"] = builder.churn_spawned
	m["compare"] = scenario.get("compare", [])
	m["expect"] = scenario.get("expect", [])

	builder.teardown()
	for _i in TEARDOWN_FRAMES:
		await process_frame
	return m


func _await_server_started() -> void:
	for _i in READY_TIMEOUT_FRAMES:
		if ResonanceServerAccess.get_server_if_initialized() != null:
			return
		await process_frame


## Each player's gate opens once the scene is ready and its source has a first direct result.
## Returns the time from playback start until every watched player may output, 0 when nothing plays, -1 on timeout.
func _await_spatial_ready(builder: RefCounted, scenario: Dictionary) -> float:
	if not _scenario_makes_sound(scenario):
		return 0.0
	var t0 := Time.get_ticks_usec()
	for _i in READY_TIMEOUT_FRAMES:
		var ready_since_usec := _spatial_ready_since_usec(builder, t0)
		if ready_since_usec >= 0:
			return float(Time.get_ticks_usec() - ready_since_usec) / 1000.0
		await process_frame
		builder.step(1.0 / FRAME_RATE)
	return -1.0


## Looping players count once all of them may output; churn scenarios count the first one-shot that may.
## Returns the play() time the wait is measured from, or -1 while still waiting.
func _spatial_ready_since_usec(builder: RefCounted, playback_start_usec: int) -> int:
	if not builder.players.is_empty():
		for p in builder.players:
			if not p.call("is_spatial_output_ready"):
				return -1
		return playback_start_usec
	for entry in builder.churn_starts:
		var p: Variant = entry["player"]
		if is_instance_valid(p) and p.call("is_spatial_output_ready"):
			return int(entry["play_usec"])
	return -1


func _scenario_makes_sound(scenario: Dictionary) -> bool:
	if not bool(scenario.get("playing", true)):
		return false
	return int(scenario.get("players", 0)) > 0 or float(scenario.get("churn_per_sec", 0.0)) > 0.0


func _advance(builder: RefCounted, seconds: float) -> void:
	var t := 0.0
	while t < seconds:
		await process_frame
		var dt := 1.0 / FRAME_RATE
		builder.step(dt)
		t += dt


func _measure(builder: RefCounted, seconds: float) -> Dictionary:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	var main_tick_us: Array[float] = []
	var process_us: Array[float] = []
	var physics_us: Array[float] = []
	var refl_us: Array[float] = []
	var direct_us: Array[float] = []
	var commit_us: Array[float] = []
	var sync_fetch_us: Array[float] = []
	var bus_us: Array[float] = []
	var rays: Array[float] = []
	var refl_age_ms: Array[float] = []
	var refl_interval_ms: Array[float] = []
	var seen_reflection_passes := -1
	var seen_age_samples := -1
	var audio := _new_audio_totals()
	_accumulate_audio(builder, audio)

	var instrumentation_before := _reverb_bus_instrumentation(srv)
	var worker_before: Dictionary = srv.get_simulation_worker_timing() if srv != null else {}
	var cpu_before := _thread_cpu.snapshot()
	var host_before := _thread_cpu.host_snapshot()
	var t0 := Time.get_ticks_usec()
	var last_frame_us := t0
	var next_audio_poll := AUDIO_POLL_SECONDS
	var frame_gaps_us: Array[float] = []
	var elapsed := 0.0
	while elapsed < seconds:
		await process_frame
		var now := Time.get_ticks_usec()
		frame_gaps_us.append(float(now - last_frame_us))
		last_frame_us = now
		var dt := 1.0 / FRAME_RATE
		builder.step(dt)
		elapsed += dt

		# Godot reports the slowest _process of the last second here, not this frame's.
		process_us.append(Performance.get_monitor(Performance.TIME_PROCESS) * 1.0e6)
		physics_us.append(Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1.0e6)
		if is_instance_valid(builder.runtime):
			main_tick_us.append(
				float(builder.runtime.get_frame_timings().get("main_thread_last_tick_usec", 0))
			)
		if srv != null:
			var w: Dictionary = srv.get_simulation_worker_timing()
			# us_run_reflections holds the last pass until the next one; sample it once per pass.
			var passes := int(w.get("reflection_passes", 0))
			if seen_reflection_passes >= 0 and passes != seen_reflection_passes:
				_append_nonzero(refl_us, w.get("us_run_reflections", 0))
			seen_reflection_passes = passes
			var age_samples := int(w.get("reflections_age_samples", 0))
			if seen_age_samples >= 0 and age_samples != seen_age_samples:
				refl_age_ms.append(float(w.get("reflections_age_us_last", 0)) / 1000.0)
			seen_age_samples = age_samples
			if int(w.get("active_realtime_reflection_sources", 0)) > 0:
				refl_interval_ms.append(_effective_reflections_interval_ms(w))
			_append_nonzero(direct_us, w.get("us_run_direct", 0))
			_append_nonzero(commit_us, w.get("us_scene_graph_commit", 0))
			_append_nonzero(sync_fetch_us, w.get("us_sync_fetch", 0))
			_append_nonzero(bus_us, srv.get_convolution_audio_timing().get("us_reverb_bus_last", 0))
			_append_nonzero(rays, w.get("shared_num_rays", 0))
		if elapsed >= next_audio_poll:
			next_audio_poll += AUDIO_POLL_SECONDS
			_accumulate_audio(builder, audio)
	_accumulate_audio(builder, audio)
	var wall_sec := float(Time.get_ticks_usec() - t0) / 1.0e6
	var cpu := _thread_cpu.usage_between(cpu_before, _thread_cpu.snapshot(), wall_sec)
	cpu["host_steal_pct"] = _thread_cpu.steal_pct_between(host_before, _thread_cpu.host_snapshot())
	var counter_rates := _lock_contention_rates(
		instrumentation_before, _reverb_bus_instrumentation(srv), wall_sec
	)
	if srv != null:
		counter_rates.merge(_pass_rates(worker_before, srv.get_simulation_worker_timing(), wall_sec))
	return _summarize(
		builder,
		srv,
		wall_sec,
		cpu,
		counter_rates,
		audio,
		{
			"main_tick_us": main_tick_us,
			"process_us": process_us,
			"physics_us": physics_us,
			"refl_us": refl_us,
			"direct_us": direct_us,
			"commit_us": commit_us,
			"sync_fetch_us": sync_fetch_us,
			"bus_us": bus_us,
			"rays": rays,
			"refl_age_ms": refl_age_ms,
			"refl_interval_ms": refl_interval_ms,
			"frame_gaps_us": frame_gaps_us,
		}
	)


func _summarize(
	builder: RefCounted,
	srv: Variant,
	wall_sec: float,
	cpu: Dictionary,
	counter_rates: Dictionary,
	audio: Dictionary,
	s: Dictionary
) -> Dictionary:
	var frame_size := int(srv.get_audio_frame_size()) if srv != null else 0
	var mix_rate := AudioServer.get_mix_rate()
	var deadline_us := 1.0e6 * frame_size / mix_rate if frame_size > 0 else 0.0
	var voices := maxi(int(audio["voices_max"]), 1)
	var blocks_per_voice := float(audio["blocks"]) / voices
	var spawn_phases: Dictionary = builder.spawn_phase_us
	var pool: Dictionary = srv.get_resource_pool_stats() if srv != null else {}
	var m := {
		"wall_sec": wall_sec,
		"frame_size": frame_size,
		"block_deadline_us": deadline_us,
		"voices": int(audio["voices_max"]),
		"main_tick_us_p50": Stats.percentile(s["main_tick_us"], 50),
		"main_tick_us_p95": Stats.percentile(s["main_tick_us"], 95),
		"main_tick_us_max": Stats.max_of(s["main_tick_us"]),
		"main_frame_cpu_us": cpu["main_pct"] / 100.0 * 1.0e6 / FRAME_RATE,
		"main_peak_us_p50": Stats.percentile(s["process_us"], 50),
		"main_peak_us_max": Stats.max_of(s["process_us"]),
		"spawn_us_p50": Stats.percentile(builder.spawn_us, 50),
		"spawn_us_max": Stats.max_of(builder.spawn_us),
		"spawn_voice_dsp_us_p50": Stats.percentile(spawn_phases["voice_dsp_us"], 50),
		"spawn_voice_dsp_us_max": Stats.max_of(spawn_phases["voice_dsp_us"]),
		"spawn_reflection_effect_us_p50":
		Stats.percentile(spawn_phases["reflection_effect_us"], 50),
		"spawn_reflection_effect_us_max": Stats.max_of(spawn_phases["reflection_effect_us"]),
		"spawn_source_us_p50": Stats.percentile(spawn_phases["source_us"], 50),
		"spawn_source_us_max": Stats.max_of(spawn_phases["source_us"]),
		"spawn_params_push_us_p50": Stats.percentile(spawn_phases["params_push_us"], 50),
		"spawn_params_push_us_max": Stats.max_of(spawn_phases["params_push_us"]),
		"stream_static_pack_us_p50": Stats.percentile(builder.stream_static_pack_us, 50),
		"stream_static_pack_us_max": Stats.max_of(builder.stream_static_pack_us),
		"stream_probe_batch_us_p50": Stats.percentile(builder.stream_probe_batch_us, 50),
		"stream_probe_batch_us_max": Stats.max_of(builder.stream_probe_batch_us),
		"material_swap_us_p50": Stats.percentile(builder.material_swap_us, 50),
		"material_swap_us_max": Stats.max_of(builder.material_swap_us),
		"pool_source_misses": int(pool.get("source_misses", 0)),
		"pool_reflection_effect_misses": int(pool.get("reflection_effect_misses", 0)),
		"main_physics_us_p95": Stats.percentile(s["physics_us"], 95),
		"players": builder.players.size(),
		"frame_gap_us_max": Stats.max_of(s["frame_gaps_us"]),
		"main_cpu_pct": cpu["main_pct"],
		"worker_cpu_pct": cpu["worker_pct"],
		"other_threads_cpu_pct": cpu["other_pct"],
		"process_cpu_pct": cpu["total_pct"],
		"host_steal_pct": cpu["host_steal_pct"],
		"worker_thread_found": cpu["worker_found"],
		"reflections_us_p50": Stats.percentile(s["refl_us"], 50),
		"reflections_us_max": Stats.max_of(s["refl_us"]),
		"reflections_age_ms_p50": Stats.percentile(s["refl_age_ms"], 50),
		"reflections_age_ms_max": Stats.max_of(s["refl_age_ms"]),
		"reflections_interval_ms_p50": Stats.percentile(s["refl_interval_ms"], 50),
		"reflections_interval_ms_max": Stats.max_of(s["refl_interval_ms"]),
		"direct_sim_us_p50": Stats.percentile(s["direct_us"], 50),
		"direct_sim_us_max": Stats.max_of(s["direct_us"]),
		"scene_commit_us_max": Stats.max_of(s["commit_us"]),
		"sync_fetch_us_p95": Stats.percentile(s["sync_fetch_us"], 95),
		"sync_fetch_us_max": Stats.max_of(s["sync_fetch_us"]),
		"reverb_bus_us_p95": Stats.percentile(s["bus_us"], 95),
		"rays_min": Stats.min_of(s["rays"]),
		"rays_max": Stats.max_of(s["rays"]),
		"audio_block_max_us": float(audio["max_block_us"]),
		"audio_block_p99_us": _block_percentile_us(audio, 99.0),
		"audio_block_p999_us": _block_percentile_us(audio, 99.9),
		"audio_stage_max_us": _stage_max_us(audio),
		"reflection_effect_apply_max_us": float(audio["reflection_effect_apply_max_us"]),
		"audio_thread_involuntary_switches": _audio_thread_switches(audio),
		"audio_us_per_voice_block": float(audio["block_us"]) / maxf(float(audio["blocks"]), 1.0),
		"audio_nexus_load_pct":
		100.0 * float(audio["block_us"]) / maxf(blocks_per_voice, 1.0) / maxf(deadline_us, 1.0),
		"audio_blocks_per_voice": blocks_per_voice,
		"audio_split_us_per_voice_block": _split_per_voice_block(audio),
		"output_underrun": int(audio["underrun"]),
		"input_dropped": int(audio["dropped"]),
		"late_mix": int(audio["late_mix"]),
	}
	if deadline_us > 0.0:
		m["audio_block_max_pct_deadline"] = 100.0 * float(audio["max_block_us"]) / deadline_us
		m["audio_block_p99_pct_deadline"] = 100.0 * m["audio_block_p99_us"] / deadline_us
		m["audio_block_p999_pct_deadline"] = 100.0 * m["audio_block_p999_us"] / deadline_us
	m.merge(counter_rates)
	return m


## Base interval plus the adaptive extra; the extra only applies while an adaptive budget is set.
func _effective_reflections_interval_ms(w: Dictionary) -> float:
	var base_ms := float(w.get("reflections_sim_interval_us", 0)) / 1000.0
	return base_ms + float(w.get("reflections_adaptive_extra_interval", 0.0)) * 1000.0


func _reverb_bus_instrumentation(srv: Variant) -> Dictionary:
	if srv == null:
		return {}
	return srv.get_reverb_bus_instrumentation()


func _lock_contention_rates(before: Dictionary, after: Dictionary, wall_sec: float) -> Dictionary:
	return _rates_per_sec(
		before, after, wall_sec, ["source_flush_try_lock_fail", "direct_passes_during_reflections"]
	)


func _rates_per_sec(before: Dictionary, after: Dictionary, wall_sec: float, keys: Array) -> Dictionary:
	var rates := {}
	for key in keys:
		var delta := int(after.get(key, 0)) - int(before.get(key, 0))
		rates[key + "_per_sec"] = float(delta) / maxf(wall_sec, 0.001)
	return rates


func _pass_rates(before: Dictionary, after: Dictionary, wall_sec: float) -> Dictionary:
	return _rates_per_sec(
		before, after, wall_sec, ["reflection_passes", "reflection_passes_skipped", "pathing_passes_skipped"]
	)


func _append_nonzero(arr: Array[float], v: Variant) -> void:
	if float(v) > 0.0:
		arr.append(float(v))


# The player counters are cumulative; reset after each read so a poll returns the delta, also for freed players.
func _accumulate_audio(builder: RefCounted, audio: Dictionary) -> void:
	var voices := 0
	for p in builder.root.get_children():
		if not p.has_method("get_audio_instrumentation"):
			continue
		var d: Dictionary = p.get_audio_instrumentation()
		p.reset_audio_instrumentation()
		if d.is_empty():
			continue
		audio["block_us"] += int(d.get("block_time_total_us", 0))
		audio["blocks"] += int(d.get("blocks_processed", 0))
		audio["max_block_us"] = maxi(audio["max_block_us"], int(d.get("max_block_time_us", 0)))
		_accumulate_block_spikes(audio, d)
		audio["underrun"] += int(d.get("output_underrun", 0))
		audio["dropped"] += int(d.get("input_dropped", 0))
		audio["late_mix"] += int(d.get("late_mix_count", 0))
		voices += int(d.get("polyphony_voice_count", 0))
		_accumulate_split(audio["split_us"], d)
	audio["voices_max"] = maxi(audio["voices_max"], voices)
	var switches := _thread_cpu.involuntary_switches(audio["audio_thread_id"])
	if switches >= 0:
		if audio["involuntary_switches_start"] < 0:
			audio["involuntary_switches_start"] = switches
		audio["involuntary_switches_end"] = switches


func _drain_audio_counters(builder: RefCounted) -> void:
	_accumulate_audio(builder, _new_audio_totals())


func _new_audio_totals() -> Dictionary:
	return {
		"block_us": 0,
		"blocks": 0,
		"max_block_us": 0,
		"underrun": 0,
		"dropped": 0,
		"late_mix": 0,
		"voices_max": 0,
		"split_us": {},
		"max_stage_us": {},
		"reflection_effect_apply_max_us": 0.0,
		"histogram": PackedInt64Array(),
		"histogram_upper_us": PackedInt64Array(),
		"audio_thread_id": -1,
		"involuntary_switches_start": -1,
		"involuntary_switches_end": -1,
	}


func _accumulate_block_spikes(audio: Dictionary, d: Dictionary) -> void:
	for key in d:
		var name := String(key)
		if name.begins_with("max_stage_"):
			audio["max_stage_us"][name] = maxf(
				float(audio["max_stage_us"].get(name, 0.0)), float(d[key])
			)
	audio["reflection_effect_apply_max_us"] = maxf(
		float(audio["reflection_effect_apply_max_us"]),
		float(d.get("reflection_effect_apply_max_us", 0.0))
	)
	var counts: PackedInt64Array = d.get("block_time_histogram", PackedInt64Array())
	var merged: PackedInt64Array = audio["histogram"]
	if merged.is_empty():
		merged.resize(counts.size())
		audio["histogram_upper_us"] = d.get("block_time_histogram_upper_us", PackedInt64Array())
	for i in mini(counts.size(), merged.size()):
		merged[i] += counts[i]
	audio["histogram"] = merged
	if audio["audio_thread_id"] < 0:
		audio["audio_thread_id"] = int(d.get("audio_thread_id", -1))


# Involuntary context switches of the audio thread in the window; -1 when /proc or the thread id is missing.
func _audio_thread_switches(audio: Dictionary) -> int:
	if audio["involuntary_switches_start"] < 0:
		return -1
	return audio["involuntary_switches_end"] - audio["involuntary_switches_start"]


# Voice block stages (stage_*_us) plus the shared reflection timers, summed over players.
func _accumulate_split(split_us: Dictionary, d: Dictionary) -> void:
	for key in d:
		var name := String(key)
		if name.begins_with("stage_") or name in REFLECTION_SPLIT_KEYS:
			split_us[name] = float(split_us.get(name, 0.0)) + float(d[key])


# A list of {key, us} rather than a dictionary: the JSON export sorts keys, and stages read best in
# pipeline order. Stages come first, then the remainder of the block, then the reflection timers.
func _split_per_voice_block(audio: Dictionary) -> Array[Dictionary]:
	var blocks := maxf(float(audio["blocks"]), 1.0)
	var stages: Array[Dictionary] = []
	var reflection: Array[Dictionary] = []
	var staged_us := 0.0
	for key in audio["split_us"]:
		var us := float(audio["split_us"][key])
		var entry := {"key": String(key), "us": us / blocks}
		if String(key).begins_with("stage_"):
			stages.append(entry)
			staged_us += us
		else:
			reflection.append(entry)
	if stages.is_empty():
		return stages
	stages.append({"key": "remainder_us", "us": (float(audio["block_us"]) - staged_us) / blocks})
	return stages + reflection


func _environment_context() -> Dictionary:
	return {
		"godot": Engine.get_version_info().get("string", ""),
		"cpu": OS.get_processor_name(),
		"cores": OS.get_processor_count(),
		"mix_rate": AudioServer.get_mix_rate(),
		"output_latency_ms": AudioServer.get_output_latency() * 1000.0,
		"audio_driver":
		AudioServer.get_driver_name() if AudioServer.has_method("get_driver_name") else "",
		"thread_cpu_supported": _thread_cpu.is_supported(),
		"date": Time.get_datetime_string_from_system(),
	}


func _load_targets() -> Dictionary:
	var text := FileAccess.get_file_as_string(TARGETS_PATH)
	var parsed: Variant = JSON.parse_string(text)
	if typeof(parsed) != TYPE_DICTIONARY:
		push_error("perf: cannot parse %s" % TARGETS_PATH)
		return {}
	return parsed


func _parse_args() -> Dictionary:
	var out := {
		"list": false,
		"report": false,
		"run": "",
		"seconds": 8.0,
		"out": "user://perf",
		"strict": false
	}
	for a in OS.get_cmdline_user_args():
		if a == "--list":
			out["list"] = true
		elif a == "--report":
			out["report"] = true
		elif a.begins_with("--run="):
			out["run"] = a.trim_prefix("--run=")
		elif a.begins_with("--seconds="):
			out["seconds"] = float(a.trim_prefix("--seconds="))
		elif a.begins_with("--out="):
			out["out"] = a.trim_prefix("--out=")
		elif a == "--strict":
			out["strict"] = true
	return out


func _block_percentile_us(audio: Dictionary, pct: float) -> float:
	return Stats.histogram_percentile(audio["histogram"], audio["histogram_upper_us"], pct)


# Longest single stage lap of any voice, as {key, us} in pipeline order like the block split.
func _stage_max_us(audio: Dictionary) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	for key in audio["max_stage_us"]:
		out.append({"key": String(key), "us": float(audio["max_stage_us"][key])})
	return out

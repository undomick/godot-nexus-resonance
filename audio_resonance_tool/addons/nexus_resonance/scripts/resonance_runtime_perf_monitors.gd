extends RefCounted
class_name ResonanceRuntimePerfMonitors

## Debugger [Performance] monitors for [ResonanceRuntime]. The runtime node owns one instance and drives it:
## [method register](owner, level), [method tick](owner) per frame, [method unregister_all] on exit. Monitor callables
## point at this instance; frame timings come from [method ResonanceRuntime.get_frame_timings], the rest is read
## straight from [ResonanceServer]. Levels [constant PERF_MONITORS_OFF]–[constant PERF_MONITORS_FULL].

const PERF_MONITORS_OFF := 0
const PERF_MONITORS_CORE := 1
const PERF_MONITORS_STANDARD := 2
const PERF_MONITORS_FULL := 3

const MON_MAIN := "Nexus Resonance/Main/runtime_last_tick_us"
const MON_MAIN_VP := "Nexus Resonance/Main/runtime_viewport_sync_us"
## Server tick slice inside [code]_process[/code]. Non-zero only at [constant PERF_MONITORS_FULL] (sub-timing of [constant MON_MAIN]).
const MON_MAIN_TICK := "Nexus Resonance/Main/runtime_server_tick_us"
const MON_MAIN_FLUSH := "Nexus Resonance/Main/runtime_flush_sources_us"
## [code]_physics_process[/code] timing. Used when Custom tracer is active; otherwise unused (still FULL-tier only).
const MON_PHYS := "Nexus Resonance/Main/runtime_physics_tick_us"
const MON_PHYS_VP := "Nexus Resonance/Main/runtime_physics_viewport_us"
const MON_PHYS_TICK := "Nexus Resonance/Main/runtime_physics_server_tick_us"
const MON_PHYS_FLUSH := "Nexus Resonance/Main/runtime_physics_flush_us"
const MON_W_SUM := "Nexus Resonance/Worker/last_tick_sum_us"

## Aggregate main/physics monitors; each row has [code]min_level[/code] ([constant PERF_MONITORS_CORE]–[constant PERF_MONITORS_FULL]).
const _MAIN_PHYS_MONITORS: Array[Dictionary] = [
	{"id": MON_MAIN, "callable": "_nexus_perf_read_main_usec", "min_level": PERF_MONITORS_CORE},
	{
		"id": MON_MAIN_VP,
		"callable": "_nexus_perf_read_main_viewport_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_MAIN_TICK,
		"callable": "_nexus_perf_read_main_tick_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_MAIN_FLUSH,
		"callable": "_nexus_perf_read_main_flush_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_PHYS,
		"callable": "_nexus_perf_read_physics_tick_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_PHYS_VP,
		"callable": "_nexus_perf_read_physics_viewport_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_PHYS_TICK,
		"callable": "_nexus_perf_read_physics_server_tick_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_PHYS_FLUSH,
		"callable": "_nexus_perf_read_physics_flush_usec",
		"min_level": PERF_MONITORS_FULL
	},
	{"id": MON_W_SUM, "callable": "_nexus_perf_read_worker_sum", "min_level": PERF_MONITORS_CORE},
]

## Worker timing keys from [method ResonanceServer.get_simulation_worker_timing] → monitor id; µs unless noted. [code]min_level[/code] per row.
const _WORKER_US_MONITORS: Array[Dictionary] = [
	{
		"id": "Nexus Resonance/Worker/direct_us",
		"key": "us_run_direct",
		"min_level": PERF_MONITORS_STANDARD
	},
	{
		"id": "Nexus Resonance/Worker/reflections_us",
		"key": "us_run_reflections",
		"min_level": PERF_MONITORS_CORE
	},
	{
		"id": "Nexus Resonance/Worker/pathing_sim_us",
		"key": "us_run_pathing",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": "Nexus Resonance/Worker/sync_fetch_total_us",
		"key": "us_sync_fetch",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": "Nexus Resonance/Worker/sync_fetch_occlusion_us",
		"key": "us_sync_fetch_occlusion",
		"min_level": PERF_MONITORS_FULL,
	},
	{
		"id": "Nexus Resonance/Worker/sync_fetch_reflections_us",
		"key": "us_sync_fetch_reflections",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": "Nexus Resonance/Worker/sync_fetch_pathing_us",
		"key": "us_sync_fetch_pathing",
		"min_level": PERF_MONITORS_FULL,
	},
	{
		"id": "Nexus Resonance/Worker/simulator_commit_us",
		"key": "us_simulator_commit",
		"min_level": PERF_MONITORS_STANDARD
	},
	{
		"id": "Nexus Resonance/Worker/scene_graph_commit_us",
		"key": "us_scene_graph_commit",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": "Nexus Resonance/Worker/dynamic_instanced_apply_us",
		"key": "us_dynamic_instanced_apply",
		"min_level": PERF_MONITORS_FULL,
	},
	{
		"id": "Nexus Resonance/Main/last_dynamic_transform_enqueue_us",
		"key": "main_last_dynamic_transform_enqueue_us",
		"min_level": PERF_MONITORS_STANDARD,
	},
	## 0/1 heavy-wake flag (not µs); legacy monitor id for dashboards.
	{
		"id": "Nexus Resonance/Worker/heavy_tick_flag",
		"key": "worker_last_wake_heavy",
		"min_level": PERF_MONITORS_CORE
	},
]

const MON_PATHING_RAN := "Nexus Resonance/Worker/pathing_ran_last_tick"
const MON_AUDIO_CONV_APPLY := "Nexus Resonance/Audio/convolution_reflection_apply_last_us"
const MON_AUDIO_CONV_BUS := "Nexus Resonance/Audio/convolution_reverb_bus_last_us"
const MON_AUDIO_MIXER_SANITIZE_AMBI := "Nexus Resonance/Audio/mixer_sanitize_ambi_last_us"
const MON_AUDIO_MIXER_SANITIZE_STEREO := "Nexus Resonance/Audio/mixer_sanitize_stereo_last_us"
## Audio-thread aggregates (~4 Hz via [ResonanceRuntime] sampling); per-player lifetime until [method ResonancePlayer.reset_audio_instrumentation].
const MON_AUDIO_OUTPUT_UNDERRUNS := "Nexus Resonance/Audio/output_underruns_total"
const MON_AUDIO_LATE_MIX := "Nexus Resonance/Audio/late_mix_total"
const MON_AUDIO_MAX_BLOCK := "Nexus Resonance/Audio/max_block_time_us"
## Peak block time across players in the last aggregate sample (lifetime max per voice until reset).
const MON_AUDIO_LAST_BLOCK := "Nexus Resonance/Audio/last_block_time_us"
const MON_AUDIO_VOICE_COUNT := "Nexus Resonance/Audio/active_voice_count"
## Active native sources (players/sources with valid config).
const MON_SERVER_SOURCE_COUNT := "Nexus Resonance/Server/active_source_count"
const MON_SERVER_PROBE_BATCH_COUNT := "Nexus Resonance/Server/active_probe_batch_count"

## Owner callables (post–worker rows): id, method on [param owner], [code]min_level[/code].
const _EXTRA_MONITORS: Array[Dictionary] = [
	{
		"id": MON_PATHING_RAN,
		"callable": "_nexus_perf_read_pathing_ran_tick",
		"min_level": PERF_MONITORS_FULL
	},
	{
		"id": MON_AUDIO_CONV_APPLY,
		"callable": "_nexus_perf_read_convolution_apply_last_us",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": MON_AUDIO_CONV_BUS,
		"callable": "_nexus_perf_read_convolution_reverb_bus_last_us",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": MON_AUDIO_MIXER_SANITIZE_AMBI,
		"callable": "_nexus_perf_read_mixer_sanitize_ambi_last_us",
		"min_level": PERF_MONITORS_FULL,
	},
	{
		"id": MON_AUDIO_MIXER_SANITIZE_STEREO,
		"callable": "_nexus_perf_read_mixer_sanitize_stereo_last_us",
		"min_level": PERF_MONITORS_FULL,
	},
	{
		"id": MON_AUDIO_OUTPUT_UNDERRUNS,
		"callable": "_nexus_perf_read_audio_output_underruns_total",
		"min_level": PERF_MONITORS_CORE,
	},
	{
		"id": MON_AUDIO_LATE_MIX,
		"callable": "_nexus_perf_read_audio_late_mix_total",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": MON_AUDIO_MAX_BLOCK,
		"callable": "_nexus_perf_read_audio_max_block_time_us",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": MON_AUDIO_LAST_BLOCK,
		"callable": "_nexus_perf_read_audio_last_block_time_us",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": MON_AUDIO_VOICE_COUNT,
		"callable": "_nexus_perf_read_audio_active_voice_count",
		"min_level": PERF_MONITORS_STANDARD,
	},
	{
		"id": MON_SERVER_SOURCE_COUNT,
		"callable": "_nexus_perf_read_server_active_source_count",
		"min_level": PERF_MONITORS_CORE,
	},
	{
		"id": MON_SERVER_PROBE_BATCH_COUNT,
		"callable": "_nexus_perf_read_server_active_probe_batch_count",
		"min_level": PERF_MONITORS_FULL,
	},
]

## Owning [ResonanceRuntime] node; set on [method register] / [method tick]. Source of [method ResonanceRuntime.get_frame_timings].
var _owner: Node = null
var _level: int = PERF_MONITORS_OFF
## Last frame timing snapshot from the owner (refreshed in [method tick]); read by the main/physics monitor callables.
var _frame_timings: Dictionary = {}

## Sampled audio aggregates ([method _sample_audio_aggregates_if_due]); per-player lifetime until [method ResonancePlayer.reset_audio_instrumentation].
var _audio_agg_output_underruns_total: int = 0
var _audio_agg_late_mix_total: int = 0
var _audio_agg_max_block_time_us: int = 0
var _audio_agg_last_block_time_us: int = 0
var _audio_agg_voice_count: int = 0
var _audio_agg_last_sample_sec: float = -1.0
const AUDIO_AGGREGATE_SAMPLE_PERIOD_SEC: float = 0.25


static func monitor_ids_for_level(level: int) -> Array[String]:
	if level <= PERF_MONITORS_OFF:
		return [] as Array[String]
	var out: Array[String] = [] as Array[String]
	var tables: Array = [_MAIN_PHYS_MONITORS, _WORKER_US_MONITORS, _EXTRA_MONITORS]
	for table in tables:
		for row in table:
			if int(row["min_level"]) <= level:
				out.append(row["id"])
	return out


## All ids ([method monitor_ids_for_level]([constant PERF_MONITORS_FULL])).
static func monitor_ids() -> Array[String]:
	return monitor_ids_for_level(PERF_MONITORS_FULL)


## Sum of selected µs fields from [method ResonanceServer.get_simulation_worker_timing] (last worker tick).
static func simulation_worker_timing_sum(w: Dictionary) -> int:
	return (
		int(w.get("us_dynamic_instanced_apply", 0))
		+ int(w.get("us_scene_graph_commit", 0))
		+ int(w.get("us_run_direct", 0))
		+ int(w.get("us_run_reflections", 0))
		+ int(w.get("us_run_pathing", 0))
		+ int(w.get("us_sync_fetch", 0))
		+ int(w.get("us_simulator_commit", 0))
	)


static func _remove_all_nexus_monitors() -> void:
	var existing: Array = Performance.get_custom_monitor_names()
	for name in existing:
		if str(name).begins_with("Nexus Resonance/"):
			Performance.remove_custom_monitor(name)


func register(owner: Node, level: int = PERF_MONITORS_STANDARD) -> void:
	_owner = owner
	_level = level
	_remove_all_nexus_monitors()
	if level <= PERF_MONITORS_OFF:
		return
	for row in _MAIN_PHYS_MONITORS:
		if int(row["min_level"]) <= level:
			Performance.add_custom_monitor(row["id"], Callable(self, row["callable"]))
	for row in _WORKER_US_MONITORS:
		if int(row["min_level"]) <= level:
			Performance.add_custom_monitor(
				row["id"], Callable(self, "_nexus_perf_read_worker_timing_field").bind(row["key"])
			)
	for row in _EXTRA_MONITORS:
		if int(row["min_level"]) <= level:
			Performance.add_custom_monitor(row["id"], Callable(self, row["callable"]))


func unregister_all() -> void:
	_level = PERF_MONITORS_OFF
	if Engine.is_editor_hint():
		return
	_remove_all_nexus_monitors()


## Refreshes the frame timing snapshot and samples audio aggregates. No-op while monitors are off.
func tick(owner: Node) -> void:
	if _level <= PERF_MONITORS_OFF:
		return
	_owner = owner
	if owner:
		var t: Variant = owner.call("get_frame_timings")
		_frame_timings = t if t is Dictionary else {}
	_sample_audio_aggregates_if_due(owner)


func _nexus_perf_read_main_usec() -> int:
	return int(_frame_timings.get("main_thread_last_tick_usec", 0))


func _nexus_perf_read_physics_tick_usec() -> int:
	return int(_frame_timings.get("runtime_physics_tick_usec", 0))


func _nexus_perf_read_main_viewport_usec() -> int:
	return int(_frame_timings.get("main_thread_viewport_usec", 0))


func _nexus_perf_read_main_tick_usec() -> int:
	return int(_frame_timings.get("main_thread_tick_usec", 0))


func _nexus_perf_read_main_flush_usec() -> int:
	return int(_frame_timings.get("main_thread_flush_usec", 0))


func _nexus_perf_read_physics_viewport_usec() -> int:
	return int(_frame_timings.get("runtime_physics_viewport_usec", 0))


func _nexus_perf_read_physics_server_tick_usec() -> int:
	return int(_frame_timings.get("runtime_physics_server_tick_usec", 0))


func _nexus_perf_read_physics_flush_usec() -> int:
	return int(_frame_timings.get("runtime_physics_flush_usec", 0))


func _nexus_perf_read_worker_sum() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_simulation_worker_timing"):
		return 0
	var w: Dictionary = srv.get_simulation_worker_timing()
	return simulation_worker_timing_sum(w)


func _nexus_perf_read_worker_timing_field(field: String) -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_simulation_worker_timing"):
		return 0
	var w: Dictionary = srv.get_simulation_worker_timing()
	var v: Variant = w.get(field, 0)
	if v is bool:
		return 1 if v else 0
	return int(v)


func _nexus_perf_read_pathing_ran_tick() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_pathing_instrumentation"):
		return 0
	var p: Dictionary = srv.get_pathing_instrumentation()
	return 1 if p.get("pathing_ran_this_tick", false) else 0


func _nexus_perf_read_convolution_apply_last_us() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_convolution_audio_timing"):
		return 0
	var t: Dictionary = srv.get_convolution_audio_timing()
	return int(t.get("us_reflection_apply_last", 0))


func _nexus_perf_read_convolution_reverb_bus_last_us() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_convolution_audio_timing"):
		return 0
	var t: Dictionary = srv.get_convolution_audio_timing()
	return int(t.get("us_reverb_bus_last", 0))


func _nexus_perf_read_mixer_sanitize_ambi_last_us() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_convolution_audio_timing"):
		return 0
	var t: Dictionary = srv.get_convolution_audio_timing()
	return int(t.get("us_mixer_sanitize_ambi_last", 0))


func _nexus_perf_read_mixer_sanitize_stereo_last_us() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_convolution_audio_timing"):
		return 0
	var t: Dictionary = srv.get_convolution_audio_timing()
	return int(t.get("us_mixer_sanitize_stereo_last", 0))


## Refresh audio aggregates from all [code]resonance_player[/code] nodes (throttled by [constant AUDIO_AGGREGATE_SAMPLE_PERIOD_SEC]).
func _sample_audio_aggregates_if_due(owner: Node) -> void:
	var now_sec := float(Time.get_ticks_msec()) * 0.001
	if (
		_audio_agg_last_sample_sec >= 0.0
		and (now_sec - _audio_agg_last_sample_sec) < AUDIO_AGGREGATE_SAMPLE_PERIOD_SEC
	):
		return
	_audio_agg_last_sample_sec = now_sec
	var tree: SceneTree = owner.get_tree() if owner else null
	if tree == null:
		_audio_agg_output_underruns_total = 0
		_audio_agg_late_mix_total = 0
		_audio_agg_max_block_time_us = 0
		_audio_agg_last_block_time_us = 0
		_audio_agg_voice_count = 0
		return
	var sum_underrun: int = 0
	var sum_late_mix: int = 0
	var max_block: int = 0
	var max_last_block: int = 0
	var sum_voices: int = 0
	for p in tree.get_nodes_in_group("resonance_player"):
		if p == null or not p.has_method("get_audio_instrumentation"):
			continue
		var d: Dictionary = p.get_audio_instrumentation()
		if d.is_empty():
			continue
		sum_underrun += int(d.get("output_underrun", 0))
		sum_late_mix += int(d.get("late_mix_count", 0))
		var mb: int = int(d.get("max_block_time_us", 0))
		if mb > max_block:
			max_block = mb
		var lb: int = int(d.get("last_block_time_us", 0))
		if lb > max_last_block:
			max_last_block = lb
		sum_voices += int(d.get("polyphony_voice_count", 0))
	_audio_agg_output_underruns_total = sum_underrun
	_audio_agg_late_mix_total = sum_late_mix
	_audio_agg_max_block_time_us = max_block
	_audio_agg_last_block_time_us = max_last_block
	_audio_agg_voice_count = sum_voices


func _nexus_perf_read_audio_output_underruns_total() -> int:
	return _audio_agg_output_underruns_total


func _nexus_perf_read_audio_late_mix_total() -> int:
	return _audio_agg_late_mix_total


func _nexus_perf_read_audio_max_block_time_us() -> int:
	return _audio_agg_max_block_time_us


func _nexus_perf_read_audio_last_block_time_us() -> int:
	return _audio_agg_last_block_time_us


func _nexus_perf_read_audio_active_voice_count() -> int:
	return _audio_agg_voice_count


func _nexus_perf_read_server_active_source_count() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_active_source_count"):
		return 0
	return int(srv.get_active_source_count())


func _nexus_perf_read_server_active_probe_batch_count() -> int:
	var srv: Variant = ResonanceServerAccess.get_server_if_initialized()
	if srv == null or not srv.has_method("get_active_probe_batch_count"):
		return 0
	return int(srv.get_active_probe_batch_count())

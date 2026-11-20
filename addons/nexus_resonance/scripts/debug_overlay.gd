extends CanvasLayer

## Runtime debug HUD: simulation, players, reverb bus and pathing health.
## Alt+1-4 fold sections, Alt+R reset counters, Alt+A list all playing players. Does not run in the editor.

const Constants = preload("resonance_config_constants.gd")
const ResonanceRuntimePerfMonitors = preload(
	"res://addons/nexus_resonance/scripts/resonance_runtime_perf_monitors.gd"
)

## Same order as ResonanceRuntimeConfig.scene_type export_enum.
const _SCENE_TYPE_LABELS: Array[String] = [
	"Default",
	"Embree",
	"Radeon Rays",
	"Custom (Godot Physics)",
]
const _AMBISONIC_ORDER_LABELS: Array[String] = ["1st order", "2nd order", "3rd order"]
const REFLECTION_CONVOLUTION := 0
const REFLECTION_PARAMETRIC := 1
const REFLECTION_HYBRID := 2

const UPDATE_INTERVAL: float = 0.2
const PLAYER_LIST_CAP: int = 8
const WIDTH_PX := 420.0
const SCROLL_HEIGHT_MIN_PX := 200.0
const SCROLL_HEIGHT_VIEWPORT_SUBTRACT_PX := 36.0

const COLOR_OK := "#44ff44"
const COLOR_WARNING := "#ffcc44"
const COLOR_ERROR := "#ff6666"
const COLOR_VALUE := "#dddddd"
const COLOR_LABEL := "#88aacc"
const COLOR_DIM := "#888888"

enum Health { OK, WARN, ISSUE }

## A playing player is an issue on buffer loss, a huge block, or a high late-mix rate; jitter alone is not.
const MAX_BLOCK_US_ISSUE := 50000
const MAX_BLOCK_US_WARN := 15000
const LATE_RATE_ISSUE_PCT := 12.0
const LATE_RATE_WARN_PCT := 2.5
const MIN_MIX_CALLS_FOR_RATE := 200
## Reflections older than this many sim intervals mean the worker cannot keep up.
const REFLECTIONS_AGE_WARN_INTERVALS := 2.0

var _scroll_host: Control
var _folds: Array[FoldableContainer] = []
var _sim_fold: FoldableContainer
var _sim_label: RichTextLabel
var _players_fold: FoldableContainer
var _players_label: RichTextLabel
var _reverb_fold: FoldableContainer
var _reverb_label: RichTextLabel
var _pathing_fold: FoldableContainer
var _pathing_label: RichTextLabel
var _update_timer: float = 0.0
var _show_all_players: bool = false
var _worker_peak_us: int = 0
var _worker_peak_stage: String = ""


func _ready() -> void:
	if Engine.is_editor_hint():
		return
	_build_ui()
	var vp := get_viewport()
	if vp:
		vp.size_changed.connect(_on_viewport_size_changed)
		call_deferred("_on_viewport_size_changed")


func _on_viewport_size_changed() -> void:
	var vp := get_viewport()
	if not vp or not is_instance_valid(_scroll_host):
		return
	var h: float = maxf(
		SCROLL_HEIGHT_MIN_PX, vp.get_visible_rect().size.y - SCROLL_HEIGHT_VIEWPORT_SUBTRACT_PX
	)
	_scroll_host.custom_minimum_size = Vector2(WIDTH_PX, h)
	_scroll_host.size = Vector2(WIDTH_PX, h)


func _build_ui() -> void:
	layer = 100
	var panel := PanelContainer.new()
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0, 0, 0, 0.2)
	style.set_corner_radius_all(2)
	style.set_content_margin_all(8)
	panel.add_theme_stylebox_override("panel", style)

	_scroll_host = Control.new()
	_scroll_host.clip_contents = true
	panel.add_child(_scroll_host)

	var scroll := ScrollContainer.new()
	scroll.set_anchors_preset(Control.PRESET_FULL_RECT)
	scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	_scroll_host.add_child(scroll)

	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 8)
	vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(vbox)

	var hint := _make_label(10)
	hint.text = (
		"%s sections  %s reset counters  %s all players"
		% [_col(COLOR_LABEL, "Alt+1-4"), _col(COLOR_LABEL, "Alt+R"), _col(COLOR_LABEL, "Alt+A")]
	)
	vbox.add_child(hint)

	_sim_fold = _add_fold(vbox, "Simulation", false)
	_sim_label = _sim_fold.get_child(0)
	_players_fold = _add_fold(vbox, "Players", false)
	_players_label = _players_fold.get_child(0)
	_reverb_fold = _add_fold(vbox, "Reverb", true)
	_reverb_label = _reverb_fold.get_child(0)
	_pathing_fold = _add_fold(vbox, "Pathing", true)
	_pathing_label = _pathing_fold.get_child(0)

	add_child(panel)
	panel.set_anchors_preset(Control.PRESET_TOP_LEFT)
	panel.position = Vector2(10, 10)
	panel.size_flags_vertical = Control.SIZE_SHRINK_BEGIN


func _make_label(font_size: int) -> RichTextLabel:
	var label := RichTextLabel.new()
	label.bbcode_enabled = true
	label.fit_content = true
	label.add_theme_font_size_override("normal_font_size", font_size)
	return label


func _add_fold(parent: Control, title: String, folded: bool) -> FoldableContainer:
	var fold := FoldableContainer.new()
	fold.title = title
	fold.folded = folded
	fold.add_child(_make_label(11))
	parent.add_child(fold)
	_folds.append(fold)
	return fold


func _unhandled_input(event: InputEvent) -> void:
	if Engine.is_editor_hint() or not visible:
		return
	var ke := event as InputEventKey
	if not ke or not ke.pressed or ke.echo or not ke.alt_pressed:
		return

	match ke.keycode:
		KEY_1, KEY_KP_1:
			_toggle_fold(0)
		KEY_2, KEY_KP_2:
			_toggle_fold(1)
		KEY_3, KEY_KP_3:
			_toggle_fold(2)
		KEY_4, KEY_KP_4:
			_toggle_fold(3)
		KEY_R:
			_reset_counters()
		KEY_A:
			_show_all_players = not _show_all_players
		_:
			return
	_update_timer = UPDATE_INTERVAL
	get_viewport().set_input_as_handled()


func _toggle_fold(i: int) -> void:
	if i < _folds.size():
		_folds[i].folded = not _folds[i].folded


func _reset_counters() -> void:
	_worker_peak_us = 0
	_worker_peak_stage = ""
	var srv: Variant = ResonanceServerAccess.get_server()
	if srv != null:
		srv.reset_reverb_bus_instrumentation()
	for p in get_tree().get_nodes_in_group("resonance_player"):
		p.reset_audio_instrumentation()


func _process(delta: float) -> void:
	if Engine.is_editor_hint() or not visible or not _sim_label:
		return
	_update_timer += delta
	if _update_timer < UPDATE_INTERVAL:
		return
	_update_timer = 0.0

	if not ResonanceServerAccess.has_server():
		_set_fold_title(_sim_fold, "Simulation - server not loaded", Health.ISSUE)
		_sim_label.text = ""
		_players_fold.visible = false
		_reverb_fold.visible = false
		_pathing_fold.visible = false
		return
	var srv: Object = ResonanceServerAccess.get_server()
	_refresh_simulation(srv)
	_players_fold.visible = true
	_refresh_players()
	_reverb_fold.visible = srv.is_initialized()
	if _reverb_fold.visible:
		_refresh_reverb(srv)
	_pathing_fold.visible = srv.is_initialized() and srv.is_pathing_enabled()
	if _pathing_fold.visible:
		_refresh_pathing(srv)


func _col(color: String, text: String) -> String:
	return "[color=%s]%s[/color]" % [color, text]


func _row(label: String, value: String) -> String:
	return "%s %s" % [_col(COLOR_LABEL, label), value]


func _health_color(health: Health) -> String:
	match health:
		Health.ISSUE:
			return COLOR_ERROR
		Health.WARN:
			return COLOR_WARNING
	return COLOR_OK


func _set_fold_title(fold: FoldableContainer, title: String, health: Health) -> void:
	# FoldableContainer.title has no BBCode; color the whole title via theme.
	fold.title = title
	fold.add_theme_color_override(
		"title_font_color", Color.from_string(_health_color(health), Color.WHITE)
	)


func _worse(a: Health, b: Health) -> Health:
	return a if a > b else b


func _ms(us: int) -> String:
	return "%.1f ms" % (us / 1000.0)


func _on_off(b: bool) -> String:
	return _col(COLOR_OK, "ON") if b else _col(COLOR_DIM, "OFF")


func _label_at(labels: Array[String], i: int) -> String:
	return labels[i] if i >= 0 and i < labels.size() else "?"


func _runtime() -> Node:
	return get_tree().get_first_node_in_group("resonance_runtime")


func _debug_players() -> Array[Node]:
	var out: Array[Node] = []
	for p in get_tree().get_nodes_in_group("resonance_player"):
		if not p.exclude_from_debug:
			out.append(p)
	return out


func _refresh_simulation(srv: Object) -> void:
	if not srv.is_initialized():
		_set_fold_title(_sim_fold, "Simulation - not initialized", Health.WARN)
		_sim_label.text = _main_thread_row()
		return

	var timing: Dictionary = srv.get_simulation_worker_timing()
	var interval_us := int(timing.get("reflections_sim_interval_us", 0))
	var worker_us := ResonanceRuntimePerfMonitors.simulation_worker_timing_sum(timing)
	var worker_health := Health.WARN if interval_us > 0 and worker_us > interval_us else Health.OK
	if not srv.is_simulating():
		_set_fold_title(_sim_fold, "Simulation - waiting for geometry", Health.WARN)
	elif worker_health == Health.WARN:
		_set_fold_title(_sim_fold, "Simulation - worker over budget", Health.WARN)
	else:
		_set_fold_title(_sim_fold, "Simulation - running", Health.OK)

	var lines: PackedStringArray = []
	(
		lines
		. append(
			_row(
				"Output",
				(
					"direct %s | reverb %s | pathing %s"
					% [
						_on_off(srv.is_output_direct_enabled()),
						_on_off(srv.is_output_reverb_enabled()),
						_on_off(srv.is_pathing_enabled()),
					]
				)
			)
		)
	)
	lines.append(_row("Setup", _setup_text(srv)))
	lines.append(_row("Scene", _scene_text(srv)))
	lines.append(_row("Sources", _sources_text(timing)))
	lines.append(_row("Rays", _rays_text(srv, timing)))
	lines.append(_row("Worker", _worker_text(timing, worker_us, worker_health)))
	lines.append(_row("Worker peak", "%s (%s)" % [_ms(_worker_peak_us), _worker_peak_stage]))
	var age_line := _reflections_age_text(timing, interval_us)
	if age_line != "":
		lines.append(_row("Refl. age", age_line))
	lines.append(_main_thread_row())
	_sim_label.text = "\n".join(lines)


func _setup_text(srv: Object) -> String:
	var refl_name := _label_at(Constants.REFLECTION_DISPLAY_NAMES, srv.get_reflection_type())
	var order_name := _label_at(_AMBISONIC_ORDER_LABELS, srv.get_ambisonic_order() - 1)
	var profile: Dictionary = srv.get_simulation_tracer_profile()
	var scene_name := _label_at(_SCENE_TYPE_LABELS, int(profile.get("scene_type", 0)))
	return (
		"%s | %s | %s | %d Hz, %d frames"
		% [refl_name, order_name, scene_name, srv.get_sample_rate(), srv.get_audio_frame_size()]
	)


func _scene_text(srv: Object) -> String:
	var triangles: int = srv.get_global_triangle_count()
	var tri_col := COLOR_WARNING if triangles == 0 else COLOR_VALUE
	return (
		"%s triangles | %d probe batches"
		% [_col(tri_col, str(triangles)), srv.get_active_probe_batch_count()]
	)


func _sources_text(timing: Dictionary) -> String:
	var players := _debug_players()
	var playing := 0
	for p in players:
		if p.is_playing():
			playing += 1
	return (
		"%d players (%d playing) | %d Steam Audio sources | reflections %d (%d realtime)"
		% [
			players.size(),
			playing,
			int(timing.get("active_source_count", 0)),
			int(timing.get("active_reflection_sources", 0)),
			int(timing.get("active_realtime_reflection_sources", 0)),
		]
	)


func _rays_text(srv: Object, timing: Dictionary) -> String:
	var rays: int = srv.get_realtime_rays()
	var text := "realtime off" if rays <= 0 else "%d realtime" % rays
	var used := int(timing.get("shared_num_rays", 0))
	if rays > 0 and used > 0 and used != rays:
		text += _col(COLOR_WARNING, " (adaptive: %d)" % used)
	return text + " | listener reverb %s" % _on_off(srv.is_listener_reverb_enabled())


func _worker_text(timing: Dictionary, worker_us: int, health: Health) -> String:
	var stages := {
		"refl": int(timing.get("us_run_reflections", 0)),
		"path": int(timing.get("us_run_pathing", 0)),
		"direct": int(timing.get("us_run_direct", 0)),
		"commit":
		int(timing.get("us_scene_graph_commit", 0)) + int(timing.get("us_simulator_commit", 0)),
		"fetch": int(timing.get("us_sync_fetch", 0)),
	}
	var top_stage := ""
	var top_us := -1
	for stage in stages:
		if stages[stage] > top_us:
			top_us = stages[stage]
			top_stage = stage
	if worker_us > _worker_peak_us:
		_worker_peak_us = worker_us
		_worker_peak_stage = top_stage

	var parts: PackedStringArray = []
	for stage in stages:
		parts.append("%s %s" % [stage, _ms(stages[stage])])
	return "%s | %s" % [_col(_health_color(health), _ms(worker_us)), " ".join(parts)]


func _reflections_age_text(timing: Dictionary, interval_us: int) -> String:
	if int(timing.get("reflections_age_samples", 0)) == 0:
		return ""
	var last_us := int(timing.get("reflections_age_us_last", 0))
	var max_us := int(timing.get("reflections_age_us_max", 0))
	var stale := interval_us > 0 and last_us > interval_us * REFLECTIONS_AGE_WARN_INTERVALS
	return (
		"%s (max %s, target %s)"
		% [
			_col(COLOR_WARNING if stale else COLOR_VALUE, _ms(last_us)),
			_ms(max_us),
			_ms(interval_us)
		]
	)


func _main_thread_row() -> String:
	var rt := _runtime()
	if not rt:
		return _row("Main thread", _col(COLOR_WARNING, "no ResonanceRuntime"))
	var t: Dictionary = rt.get_frame_timings()
	var text := "tick %s" % _ms(int(t.get("main_thread_last_tick_usec", 0)))
	var physics_us := int(t.get("runtime_physics_tick_usec", 0))
	if physics_us > 0:
		text += " | custom scene physics %s" % _ms(physics_us)
	return _row("Main thread", text)


func _refresh_players() -> void:
	var players := _debug_players()
	if players.is_empty():
		_set_fold_title(_players_fold, "Players - none", Health.WARN)
		_players_label.text = ""
		return

	var counts := {Health.OK: 0, Health.WARN: 0, Health.ISSUE: 0}
	var entries: Array[Dictionary] = []
	for p in players:
		if not p.is_playing():
			continue
		var inst: Dictionary = p.get_audio_instrumentation()
		var health := _player_health(inst)
		counts[health] += 1
		if health != Health.OK or _show_all_players:
			entries.append({"player": p, "inst": inst, "health": health})
	entries.sort_custom(func(a, b): return a.health > b.health)

	var listed: PackedStringArray = []
	for e in entries.slice(0, PLAYER_LIST_CAP):
		listed.append(_player_line(e.player, e.inst, e.health))

	var playing: int = counts[Health.OK] + counts[Health.WARN] + counts[Health.ISSUE]
	var title := "Players - %d playing" % playing
	var title_health := Health.OK
	if counts[Health.ISSUE] > 0:
		title += ", %d with issues" % counts[Health.ISSUE]
		title_health = Health.ISSUE
	elif counts[Health.WARN] > 0:
		title += ", %d to watch" % counts[Health.WARN]
		title_health = Health.WARN
	_set_fold_title(_players_fold, title, title_health)

	var lines: PackedStringArray = []
	if listed.is_empty():
		var msg := "no player is playing" if playing == 0 else "all playing players OK"
		lines.append(_col(COLOR_DIM if playing == 0 else COLOR_OK, msg))
	else:
		lines.append_array(listed)
	lines.append(_col(COLOR_DIM, "Counters run since start or Alt+R."))
	_players_label.text = "\n".join(lines)


func _late_rate_pct(inst: Dictionary) -> float:
	var mix_calls := int(inst.get("mix_calls", 0))
	if mix_calls < MIN_MIX_CALLS_FOR_RATE:
		return 0.0
	return 100.0 * int(inst.get("late_mix_count", 0)) / mix_calls


func _buffer_losses(inst: Dictionary) -> int:
	return (
		int(inst.get("input_dropped", 0))
		+ int(inst.get("output_underrun", 0))
		+ int(inst.get("output_blocked", 0))
	)


func _player_health(inst: Dictionary) -> Health:
	if inst.is_empty():
		return Health.WARN
	var late_pct := _late_rate_pct(inst)
	var max_block_us := int(inst.get("max_block_time_us", 0))
	if (
		_buffer_losses(inst) > 0
		or max_block_us >= MAX_BLOCK_US_ISSUE
		or late_pct >= LATE_RATE_ISSUE_PCT
	):
		return Health.ISSUE
	if late_pct >= LATE_RATE_WARN_PCT or max_block_us >= MAX_BLOCK_US_WARN:
		return Health.WARN
	return Health.OK


func _player_line(p: Node, inst: Dictionary, health: Health) -> String:
	var badge := _col(_health_color(health), ["OK", "WARN", "ISSUE"][health])
	if inst.is_empty():
		return "%s %s  no instrumentation data" % [badge, p.name]

	var parts: PackedStringArray = []
	for key in ["input_dropped", "output_underrun", "output_blocked"]:
		var n := int(inst.get(key, 0))
		if n > 0:
			parts.append(_col(COLOR_ERROR, "%s %d" % [key.replace("_", " "), n]))
	parts.append("late %.1f%%" % _late_rate_pct(inst))
	parts.append("max block %s" % _ms(int(inst.get("max_block_time_us", 0))))
	parts.append("rms %.3f" % float(inst.get("last_output_rms", 0.0)))
	var voices := int(inst.get("polyphony_voice_count", 0))
	if voices > 1:
		parts.append("%d voices" % voices)
	return "%s %s  %s" % [badge, p.name, " | ".join(parts)]


func _refresh_reverb(srv: Object) -> void:
	var ri: Dictionary = srv.get_reverb_bus_instrumentation()
	var refl_type := int(ri.get("reflection_type", -1))
	var type_name := _label_at(Constants.REFLECTION_DISPLAY_NAMES, refl_type)
	var lines: PackedStringArray = []

	if refl_type == REFLECTION_PARAMETRIC or refl_type == REFLECTION_HYBRID:
		_set_fold_title(_reverb_fold, "Reverb - %s" % type_name, Health.OK)
		lines.append(
			_col(COLOR_DIM, "Wet signal is mixed in each player; the reverb bus is unused.")
		)
		lines.append(_output_bus_row())
		_reverb_label.text = "\n".join(lines)
		return

	var health := Health.OK
	var mixer_ok: bool = ri.get("mixer_exists", false)
	var calls := int(ri.get("effect_process_calls", 0))
	var success := int(ri.get("effect_success", 0))
	var effect_health := Health.OK if mixer_ok and calls > 0 and success == calls else Health.WARN
	health = _worse(health, effect_health)
	(
		lines
		. append(
			_row(
				"Bus effect",
				(
					"%s | %d mixer feeds | wet rms %.3f peak %.3f"
					% [
						_col(_health_color(effect_health), "%d / %d ok" % [success, calls]),
						int(ri.get("mixer_feed_count", 0)),
						float(ri.get("effect_output_rms_pre_gain", 0.0)),
						float(ri.get("effect_output_peak", 0.0)),
					]
				)
			)
		)
	)

	var hit := int(ri.get("fetch_cache_hit", 0))
	var miss := int(ri.get("fetch_cache_miss", 0))
	var miss_pct := 100.0 * miss / (hit + miss) if hit + miss > 0 else 0.0
	var fetch_text := "%.1f%% miss" % miss_pct
	if refl_type == REFLECTION_CONVOLUTION:
		var ir_null := int(ri.get("convolution_feed_ir_null", 0))
		if ir_null > 0:
			health = _worse(health, Health.WARN)
		fetch_text += (
			" | IR feeds %d ok, %s"
			% [
				int(ri.get("convolution_valid_fetches", 0)),
				_col(COLOR_WARNING if ir_null > 0 else COLOR_VALUE, "%d without IR" % ir_null),
			]
		)
	lines.append(_row("Fetch", fetch_text))

	var anomalies := _reverb_anomalies(ri)
	if anomalies != "":
		health = _worse(health, Health.WARN)
		lines.append(_row("Anomalies", _col(COLOR_WARNING, anomalies)))

	lines.append(_output_bus_row())
	_set_fold_title(_reverb_fold, "Reverb - %s" % type_name, health)
	_reverb_label.text = "\n".join(lines)


func _reverb_anomalies(ri: Dictionary) -> String:
	var parts: PackedStringArray = []
	var hold_last := int(ri.get("mixer_return_hold_last_count", 0))
	if hold_last > 0:
		parts.append("mixer held last block %dx" % hold_last)
	var click_guard := int(ri.get("effect_click_guard_triggers", 0))
	if click_guard > 0:
		parts.append("click guard %dx" % click_guard)
	var deferred := int(ri.get("mixer_deferred_pending", 0))
	if deferred > 0:
		parts.append("%d mixer releases pending" % deferred)
	return " | ".join(parts)


func _output_bus_row() -> String:
	var rt := _runtime()
	if not rt:
		return _row("Output bus", _col(COLOR_WARNING, "no ResonanceRuntime"))
	var text := str(rt.get_bus_effective())
	var ai: Dictionary = rt.activator_instrumentation
	if not ai.is_empty():
		if ai.get("active", false):
			text += " | activator %s" % _col(COLOR_OK, "active")
		else:
			text += " | activator %s" % _col(COLOR_WARNING, str(ai.get("reason", "inactive")))
	return _row("Output bus", text)


func _refresh_pathing(srv: Object) -> void:
	var pi: Dictionary = srv.get_pathing_instrumentation()
	var attempts := int(pi.get("sim_attempt", 0))
	var ran := int(pi.get("sim_ran", 0))
	var crashed := int(pi.get("sim_seh_fail", 0))
	var sh_ok := int(pi.get("fetch_sh_ok", 0))
	var sh_null := int(pi.get("fetch_sh_null", 0))
	var applied := int(pi.get("player_applied", 0))
	var missed := int(pi.get("player_fetch_miss", 0))

	var health := Health.OK
	if (attempts > 0 and ran == 0) or (missed > 0 and sh_null > 0 and applied == 0):
		health = Health.ISSUE
	elif crashed > 0 or (missed > 0 and applied == 0):
		health = Health.WARN
	_set_fold_title(_pathing_fold, "Pathing", health)

	var sim_text := (
		"%d / %d ran | skipped: no listener %d, cooldown %d"
		% [ran, attempts, int(pi.get("sim_skip_listener", 0)), int(pi.get("sim_skip_cooldown", 0))]
	)
	if crashed > 0:
		sim_text += " | %s" % _col(COLOR_ERROR, "%d crashed" % crashed)
	var lines: PackedStringArray = []
	lines.append(_row("Simulation", sim_text))
	lines.append(
		_row(
			"Fetch",
			(
				"%d ok | %s"
				% [sh_ok, _col(COLOR_WARNING if sh_null > 0 else COLOR_VALUE, "%d empty" % sh_null)]
			)
		)
	)
	(
		lines
		. append(
			_row(
				"Players",
				(
					"%d applied / %d gated | %s"
					% [
						applied,
						int(pi.get("player_gate", 0)),
						_col(COLOR_WARNING if missed > 0 else COLOR_VALUE, "%d missed" % missed),
					]
				)
			)
		)
	)
	_pathing_label.text = "\n".join(lines)

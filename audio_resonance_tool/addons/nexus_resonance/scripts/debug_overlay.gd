extends CanvasLayer

## Runtime debug HUD (server, audio instrumentation, reverb). Alt+1–3 folds, Alt+R reset, Alt+A details.
## Does not run in the editor. Reflection-line overlay needs realtime rays + debug flags on the player.

const Constants = preload("resonance_config_constants.gd")

## Same order as ResonanceRuntimeConfig.scene_type export_enum.
const _SCENE_TYPE_LABELS: Array[String] = [
	"Default",
	"Embree",
	"Radeon Rays",
	"Custom (Godot Physics)",
]

const UPDATE_INTERVAL: float = 0.2
const AUDIO_PROBLEM_DETAIL_CAP: int = 5
const COLOR_OK := "#44ff44"
const COLOR_WARNING := "#ffcc44"
const COLOR_ERROR := "#ff6666"
const COLOR_NEUTRAL := "#dddddd"
const COLOR_HINT := "#88aacc"
## Instrumentation: "issue" only on buffer loss, huge blocks, or high late-mix % (not every jitter).
const AUDIO_INST_MAX_BLOCK_US_ISSUE := 50000
const AUDIO_INST_LATE_RATE_ISSUE_PCT := 12.0
const AUDIO_INST_MIN_MIX_CALLS_FOR_RATE := 200
const AUDIO_INST_LATE_RATE_WARN_PCT := 2.5
const AUDIO_INST_MAX_BLOCK_US_WARN := 15000
## Scroll height: viewport minus fixed chrome (not a % of window).
const OVERLAY_SCROLL_HEIGHT_MIN_PX := 200.0
const OVERLAY_SCROLL_HEIGHT_VIEWPORT_SUBTRACT_PX := 36.0

var _panel: PanelContainer
## Sized to viewport height so ScrollContainer can show all sections.
var _scroll_host: Control
var _outer_scroll: ScrollContainer
var _vbox: VBoxContainer
var _hint_label: RichTextLabel
var _folds: Array[FoldableContainer] = []
var _fold_server: FoldableContainer
var _status_label: RichTextLabel
var _audio_instrumentation_label: RichTextLabel
var _reverb_compact_label: RichTextLabel
var _update_timer: float = 0.0
var _audio_show_details: bool = false
var _worker_spike_hold_until_ms: int = 0
var _worker_spike_hold_line: String = ""


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
	var vs: Vector2 = vp.get_visible_rect().size
	var h: float = maxf(
		OVERLAY_SCROLL_HEIGHT_MIN_PX, vs.y - OVERLAY_SCROLL_HEIGHT_VIEWPORT_SUBTRACT_PX
	)
	var w: float = 420.0
	_scroll_host.custom_minimum_size = Vector2(w, h)
	_scroll_host.size = Vector2(w, h)


func _build_ui() -> void:
	layer = 100
	_panel = PanelContainer.new()
	var style = StyleBoxFlat.new()
	style.bg_color = Color(0, 0, 0, 0.2)
	style.border_color = Color(0.4, 0.6, 0.9)
	style.set_border_width_all(0)
	style.set_corner_radius_all(2)
	style.set_content_margin_all(8)
	_panel.add_theme_stylebox_override("panel", style)

	_scroll_host = Control.new()
	_scroll_host.clip_contents = true
	_panel.add_child(_scroll_host)

	_outer_scroll = ScrollContainer.new()
	_outer_scroll.set_anchors_preset(Control.PRESET_FULL_RECT)
	_outer_scroll.offset_left = 0
	_outer_scroll.offset_top = 0
	_outer_scroll.offset_right = 0
	_outer_scroll.offset_bottom = 0
	_outer_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	_outer_scroll.vertical_scroll_mode = ScrollContainer.SCROLL_MODE_AUTO
	_scroll_host.add_child(_outer_scroll)

	_vbox = VBoxContainer.new()
	_vbox.add_theme_constant_override("separation", 8)
	_vbox.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_outer_scroll.add_child(_vbox)

	_hint_label = RichTextLabel.new()
	_hint_label.bbcode_enabled = true
	_hint_label.fit_content = true
	_hint_label.add_theme_font_size_override("normal_font_size", 10)
	# Avoid % string operator: en-dash or other chars can confuse the formatter.
	var ch := COLOR_HINT
	_hint_label.text = (
		"[color="
		+ ch
		+ "]Alt+1-3[/color] sections  [color="
		+ ch
		+ "]Alt+R[/color] reset  "
		+ "[color="
		+ ch
		+ "]Alt+A[/color] audio details\n"
	)
	_vbox.add_child(_hint_label)

	var fold_server := FoldableContainer.new()
	_fold_server = fold_server
	fold_server.title = "Server Status"
	fold_server.folded = false
	_folds.append(fold_server)
	_status_label = RichTextLabel.new()
	_status_label.bbcode_enabled = true
	_status_label.fit_content = true
	_status_label.add_theme_font_size_override("normal_font_size", 12)
	_status_label.text = "Nexus Resonance Debug"
	fold_server.add_child(_status_label)
	_vbox.add_child(fold_server)

	var fold_audio := FoldableContainer.new()
	fold_audio.title = "Audio Instrumentation"
	fold_audio.folded = true
	_folds.append(fold_audio)
	_audio_instrumentation_label = RichTextLabel.new()
	_audio_instrumentation_label.bbcode_enabled = true
	_audio_instrumentation_label.fit_content = true
	_audio_instrumentation_label.add_theme_font_size_override("normal_font_size", 11)
	_audio_instrumentation_label.text = "(No data)"
	fold_audio.add_child(_audio_instrumentation_label)
	_vbox.add_child(fold_audio)

	var fold_reverb := FoldableContainer.new()
	fold_reverb.title = "Reverb Bus"
	fold_reverb.folded = true
	_folds.append(fold_reverb)
	var reverb_vbox := VBoxContainer.new()
	reverb_vbox.add_theme_constant_override("separation", 4)
	_reverb_compact_label = RichTextLabel.new()
	_reverb_compact_label.bbcode_enabled = true
	_reverb_compact_label.fit_content = true
	_reverb_compact_label.add_theme_font_size_override("normal_font_size", 11)
	_reverb_compact_label.text = "(No data)"
	reverb_vbox.add_child(_reverb_compact_label)
	fold_reverb.add_child(reverb_vbox)
	_vbox.add_child(fold_reverb)

	add_child(_panel)
	_panel.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_panel.position = Vector2(10, 10)
	_panel.custom_minimum_size = Vector2(400, 0)
	_panel.size_flags_vertical = Control.SIZE_SHRINK_BEGIN


func _unhandled_input(event: InputEvent) -> void:
	if Engine.is_editor_hint() or not visible:
		return
	if not event is InputEventKey:
		return
	var ke := event as InputEventKey
	if not ke.pressed or ke.echo:
		return
	if not ke.alt_pressed:
		return

	var handled := true
	match ke.keycode:
		KEY_1, KEY_KP_1:
			_toggle_fold_index(0)
		KEY_2, KEY_KP_2:
			_toggle_fold_index(1)
		KEY_3, KEY_KP_3:
			_toggle_fold_index(2)
		KEY_R:
			_reset_meters()
		KEY_A:
			_audio_show_details = not _audio_show_details
			_update_timer = UPDATE_INTERVAL
		_:
			handled = false

	if handled:
		get_viewport().set_input_as_handled()


func _toggle_fold_index(i: int) -> void:
	if i < 0 or i >= _folds.size():
		return
	_folds[i].folded = not _folds[i].folded


func _reset_meters() -> void:
	var srv_reset: Variant = ResonanceServerAccess.get_server()
	if srv_reset != null and srv_reset.has_method("reset_reverb_bus_instrumentation"):
		srv_reset.reset_reverb_bus_instrumentation()
	var tree = get_tree()
	if tree:
		for p in tree.get_nodes_in_group("resonance_player"):
			if p.has_method("reset_audio_instrumentation"):
				p.reset_audio_instrumentation()


func _process(delta: float) -> void:
	if Engine.is_editor_hint():
		return
	if not visible:
		return
	if not _panel or not _status_label:
		return

	_update_timer += delta
	if _update_timer >= UPDATE_INTERVAL:
		_update_timer = 0.0
		_refresh_status()
		_refresh_audio_instrumentation()
		_refresh_reverb_bus()


func _refresh_status() -> void:
	if not _fold_server:
		return
	if not ResonanceServerAccess.has_server():
		_status_label.text = "[color=%s]ResonanceServer not loaded[/color]" % COLOR_ERROR
		_fold_server.title = "Server Status - not loaded"
		_fold_server.add_theme_color_override(
			"title_font_color", Color.from_string(COLOR_ERROR, Color.RED)
		)
		return

	var srv = ResonanceServerAccess.get_server()
	var init_ok: bool = srv.is_initialized()
	var sim_ok: bool = srv.is_simulating() if init_ok else false
	# FoldableContainer.title has no BBCode; color the whole title via theme.
	if not init_ok:
		_fold_server.title = "Server Status - not initialized"
		_fold_server.add_theme_color_override(
			"title_font_color", Color.from_string(COLOR_WARNING, Color.ORANGE)
		)
	elif not sim_ok:
		_fold_server.title = "Server Status - waiting for geometry"
		_fold_server.add_theme_color_override(
			"title_font_color", Color.from_string(COLOR_WARNING, Color.ORANGE)
		)
	else:
		_fold_server.title = "Server Status - initialized"
		_fold_server.add_theme_color_override(
			"title_font_color", Color.from_string(COLOR_OK, Color.LIME_GREEN)
		)

	var parts: PackedStringArray = []
	var path_on: bool = srv.is_pathing_enabled() if srv.has_method("is_pathing_enabled") else false
	parts.append(
		(
			"[color=%s]Output:[/color] Direct - %s | Reverb - %s | Pathing - %s"
			% [
				COLOR_NEUTRAL,
				_str_bool(srv.is_output_direct_enabled()),
				_str_bool(srv.is_output_reverb_enabled()),
				_str_bool(path_on)
			]
		)
	)

	var ao: int = 1
	if srv.has_method("get_ambisonic_order"):
		ao = int(srv.get_ambisonic_order())
	parts.append(
		"[color=%s]Ambisonic Order:[/color] %s" % [COLOR_NEUTRAL, _ambisonic_order_label(ao)]
	)

	var scene_idx: int = 0
	if srv.has_method("get_simulation_tracer_profile"):
		var prof: Dictionary = srv.get_simulation_tracer_profile()
		scene_idx = int(prof.get("scene_type", 0))
	var scene_lbl: String = (
		_SCENE_TYPE_LABELS[scene_idx]
		if scene_idx >= 0 and scene_idx < _SCENE_TYPE_LABELS.size()
		else "?"
	)
	parts.append("[color=%s]Scene Type:[/color] %s" % [COLOR_NEUTRAL, scene_lbl])

	var refl_type: int = srv.get_reflection_type() if srv.has_method("get_reflection_type") else 0
	var names = Constants.REFLECTION_DISPLAY_NAMES
	var refl_name: String = names[refl_type] if refl_type >= 0 and refl_type < names.size() else "?"
	parts.append("[color=%s]Reflection Type:[/color] %s" % [COLOR_NEUTRAL, refl_name])

	var drm: int = 0
	if srv.has_method("get_default_reflections_mode"):
		drm = int(srv.get_default_reflections_mode())
	var rays: int = int(srv.get_realtime_rays()) if srv.has_method("get_realtime_rays") else 0
	var mode_base: String = "Baked" if drm == 0 else "Realtime"
	var refl_mode_line: String
	if rays > 0:
		refl_mode_line = "%s | %d realtime rays" % [mode_base, rays]
	else:
		refl_mode_line = "%s | realtime rays off" % mode_base
	parts.append("[color=%s]Reflection Mode:[/color] %s" % [COLOR_NEUTRAL, refl_mode_line])

	if refl_type == 1 or refl_type == 2:
		parts.append("[color=%s]Parametric/Hybrid: reverb in player output[/color]" % COLOR_WARNING)

	var tree := get_tree()
	var rt: Node = tree.get_first_node_in_group("resonance_runtime") if tree else null
	var mtu := 0
	var ptu := 0
	if rt:
		var _mt: Variant = rt.get("main_thread_last_tick_usec")
		mtu = int(_mt) if _mt != null else 0
		var _pt: Variant = rt.get("runtime_physics_tick_usec")
		ptu = int(_pt) if _pt != null else 0
	var w_sum := 0
	var w_heavy_tag := ""
	var wtim: Dictionary = {}
	if init_ok and srv.has_method("get_simulation_worker_timing"):
		wtim = srv.get_simulation_worker_timing()
		w_sum = ResonanceRuntimePerfMonitors.simulation_worker_timing_sum(wtim)
		var wh: Variant = wtim.get("worker_last_wake_heavy", false)
		var w_heavy: bool = wh if wh is bool else bool(wh)
		w_heavy_tag = "H" if w_heavy else "L"
	if init_ok:
		(
			parts
			. append(
				(
					"[color=%s]Performance:[/color] [color=%s]%d µs[/color] | worker Σ [color=%s]%d µs[/color] [color=%s](%s)[/color]"
					% [
						COLOR_NEUTRAL,
						COLOR_NEUTRAL,
						mtu,
						COLOR_NEUTRAL,
						w_sum,
						COLOR_HINT,
						w_heavy_tag
					]
				)
			)
		)
		# Show a quick breakdown when the worker spikes (helps diagnose "absurd" values).
		var now_ms: int = Time.get_ticks_msec()
		var spike_line: String = ""
		if w_sum >= 10000 and not wtim.is_empty():
			var keys := [
				"us_run_reflections",
				"us_run_pathing",
				"us_scene_graph_commit",
				"us_simulator_commit",
				"us_sync_fetch",
				"us_dynamic_instanced_apply",
				"us_run_direct",
			]
			var best_key := ""
			var best_val := -1
			for k in keys:
				var v := int(wtim.get(k, 0))
				if v > best_val:
					best_val = v
					best_key = k
			if best_key != "":
				spike_line = (
					"[color=%s]Worker spike:[/color] top=%s %d µs | sceneCommit=%d µs | refl=%d µs | path=%d µs | sync=%d µs"
					% [
						COLOR_HINT,
						best_key,
						best_val,
						int(wtim.get("us_scene_graph_commit", 0)),
						int(wtim.get("us_run_reflections", 0)),
						int(wtim.get("us_run_pathing", 0)),
						int(wtim.get("us_sync_fetch", 0)),
					]
				)
				_worker_spike_hold_line = spike_line
				_worker_spike_hold_until_ms = now_ms + 2000
		# Hold the last spike line for easier screenshots.
		if spike_line != "":
			parts.append(spike_line)
		elif _worker_spike_hold_line != "" and now_ms < _worker_spike_hold_until_ms:
			parts.append(_worker_spike_hold_line)
		# Diagnostics: explain whether we're actually tracing realtime reflection rays.
		if not wtim.is_empty():
			var nr := int(wtim.get("shared_num_rays", -1))
			var art := int(wtim.get("shared_adaptive_num_rays_target", -1))
			var ar := int(wtim.get("active_reflection_sources", -1))
			var rr := int(wtim.get("active_realtime_reflection_sources", -1))
			if nr >= 0 or ar >= 0 or rr >= 0:
				var extra := ""
				if art >= 0:
					extra = " | adaptive_target=%d" % maxi(art, 0)
				parts.append(
					(
						"[color=%s]Reflections:[/color] active=%d realtime=%d | shared numRays=%d%s"
						% [COLOR_HINT, maxi(ar, 0), maxi(rr, 0), maxi(nr, 0), extra]
					)
				)
	else:
		(
			parts
			. append(
				(
					"[color=%s]Performance:[/color] [color=%s]%d µs[/color] | worker [color=%s]-[/color]"
					% [COLOR_NEUTRAL, COLOR_NEUTRAL, mtu, COLOR_HINT]
				)
			)
		)
	if ptu > 0:
		parts.append(
			(
				"[color=%s]Custom scene[/color] [i]_physics_process[/i] [color=%s]%d µs[/color]"
				% [COLOR_NEUTRAL, COLOR_NEUTRAL, ptu]
			)
		)

	_status_label.text = "\n".join(parts)


func _ambisonic_order_label(order: int) -> String:
	var o := clampi(order, 1, 3)
	match o:
		1:
			return "1st Order"
		2:
			return "2nd Order"
		3:
			return "3rd Order"
		_:
			return "%d (order)" % order


func _audio_inst_col_buf(n: int) -> String:
	return COLOR_OK if n == 0 else COLOR_ERROR


func _audio_inst_col_max_block_us(us: int, is_issue: bool) -> String:
	if us <= 0:
		return COLOR_OK
	if is_issue or us >= AUDIO_INST_MAX_BLOCK_US_ISSUE:
		return COLOR_ERROR
	if us >= AUDIO_INST_MAX_BLOCK_US_WARN:
		return COLOR_WARNING
	return COLOR_OK


func _audio_inst_col_late_mix(
	late_mix: int, mix_calls: int, is_issue: bool, is_warn: bool
) -> String:
	if late_mix <= 0:
		return COLOR_OK
	if is_issue:
		return COLOR_ERROR
	if is_warn:
		return COLOR_WARNING
	if mix_calls < AUDIO_INST_MIN_MIX_CALLS_FOR_RATE:
		return COLOR_NEUTRAL
	return COLOR_OK


func _classify_player_instrumentation(
	p: Node, inst: Dictionary, currently_playing: bool
) -> Dictionary:
	# bucket: idle | running_no_data | running_ok | running_issue. ui_severity: 0 / 1 / 2
	var detail_lines: PackedStringArray = []
	if not currently_playing:
		return {
			"bucket": &"idle",
			"status_ok": true,
			"detail": detail_lines,
			"name": p.name,
			"ui_severity": 0,
		}
	if inst.is_empty():
		detail_lines.append(
			(
				"[color=%s]%s[/color]  [color=%s](no instrumentation snapshot)[/color]"
				% [COLOR_NEUTRAL, p.name, COLOR_WARNING]
			)
		)
		return {
			"bucket": &"running_no_data",
			"status_ok": false,
			"detail": detail_lines,
			"name": p.name,
			"ui_severity": 2,
		}

	var input_dropped := int(inst.get("input_dropped", 0))
	var output_underrun := int(inst.get("output_underrun", 0))
	var output_blocked := int(inst.get("output_blocked", 0))
	var max_block_us := int(inst.get("max_block_time_us", 0))
	var late_mix := int(inst.get("late_mix_count", 0))
	var mix_calls := int(inst.get("mix_calls", 0))
	var buf_ok := input_dropped == 0 and output_underrun == 0 and output_blocked == 0
	var late_rate_pct := 100.0 * float(late_mix) / float(max(1, mix_calls))

	var buf_issue := not buf_ok
	var max_block_issue := max_block_us >= AUDIO_INST_MAX_BLOCK_US_ISSUE
	var late_rate_issue := (
		mix_calls >= AUDIO_INST_MIN_MIX_CALLS_FOR_RATE
		and late_rate_pct >= AUDIO_INST_LATE_RATE_ISSUE_PCT
	)
	var is_issue := buf_issue or max_block_issue or late_rate_issue

	var late_rate_warn := (
		buf_ok
		and not is_issue
		and mix_calls >= AUDIO_INST_MIN_MIX_CALLS_FOR_RATE
		and late_rate_pct >= AUDIO_INST_LATE_RATE_WARN_PCT
	)
	var ui_severity := 2 if is_issue else (1 if late_rate_warn else 0)

	var c_drop := _audio_inst_col_buf(input_dropped)
	var c_un := _audio_inst_col_buf(output_underrun)
	var c_blk := _audio_inst_col_buf(output_blocked)
	var c_late := _audio_inst_col_late_mix(late_mix, mix_calls, is_issue, late_rate_warn)
	var c_max := _audio_inst_col_max_block_us(max_block_us, max_block_issue)
	var max_ms := "%.1f" % (max_block_us / 1000.0)

	var passthrough := int(inst.get("passthrough_blocks", 0))
	var reverb_miss := int(inst.get("reverb_miss_blocks", 0))
	var param_syncs := int(inst.get("param_sync_count", 0))
	var zero_input := int(inst.get("zero_input_count", 0))
	var silent := int(inst.get("silent_output_blocks", 0))
	var last_rms := float(inst.get("last_output_rms", 0.0))
	var path_sh_rms := float(inst.get("pathing_sh_rms", 0.0))
	var path_sh_energy := float(inst.get("pathing_sh_energy", 0.0))
	var path_out_rms := float(inst.get("pathing_out_rms", 0.0))
	var path_sh_order := int(inst.get("pathing_sh_order", -1))

	var badge := "[color=%s]OK[/color]" % COLOR_OK
	if is_issue:
		badge = "[color=%s]ISSUE[/color]" % COLOR_ERROR
	elif ui_severity == 1:
		badge = "[color=%s]WARN[/color]" % COLOR_WARNING

	(
		detail_lines
		. append(
			(
				"[color=%s]%s[/color]  buf drop=[color=%s]%d[/color] underrun=[color=%s]%d[/color] blocked=[color=%s]%d[/color] | late=[color=%s]%d[/color] (%.2f%% of mix) max=[color=%s]%s[/color]ms"
				% [
					COLOR_NEUTRAL,
					p.name,
					c_drop,
					input_dropped,
					c_un,
					output_underrun,
					c_blk,
					output_blocked,
					c_late,
					late_mix,
					late_rate_pct,
					c_max,
					max_ms,
				]
			)
		)
	)
	(
		detail_lines
		. append(
			(
				"  [color=%s]proc[/color] passthru=%d rmiss=%d silent=%d zero_in=%d rms=%.4f psync=%d  %s"
				% [
					COLOR_NEUTRAL,
					passthrough,
					reverb_miss,
					silent,
					zero_input,
					last_rms,
					param_syncs,
					badge
				]
			)
		)
	)
	# No printf %e in GDScript; use String.num_scientific for energy.
	var path_energy_str: String = String.num_scientific(path_sh_energy)
	(
		detail_lines
		. append(
			(
				"  [color=%s]pathing[/color] sh_rms=%s sh_energy=%s out_rms=%s order=%d"
				% [
					COLOR_NEUTRAL,
					String.num(path_sh_rms, 4),
					path_energy_str,
					String.num(path_out_rms, 4),
					path_sh_order,
				]
			)
		)
	)

	if is_issue:
		return {
			"bucket": &"running_issue",
			"status_ok": false,
			"detail": detail_lines,
			"name": p.name,
			"ui_severity": 2,
		}
	return {
		"bucket": &"running_ok",
		"status_ok": true,
		"detail": detail_lines,
		"name": p.name,
		"ui_severity": ui_severity,
	}


func _refresh_audio_instrumentation() -> void:
	if not _audio_instrumentation_label:
		return
	var tree = get_tree()
	if not tree:
		return
	var players = tree.get_nodes_in_group("resonance_player")
	if players.is_empty():
		_audio_instrumentation_label.text = (
			"[color=%s]No ResonancePlayer nodes[/color]" % COLOR_WARNING
		)
		return

	var block_size := -1
	var srv_audio: Variant = ResonanceServerAccess.get_server()
	if srv_audio != null and srv_audio.has_method("get_audio_frame_size"):
		block_size = srv_audio.get_audio_frame_size()

	var idle_n := 0
	var run_no_data := 0
	var run_issue := 0
	var run_ok := 0
	var run_watch := 0
	var problem_details: Array[Dictionary] = []
	var watch_details: Array[Dictionary] = []

	for p in players:
		if p.get("exclude_from_debug") == true:
			continue
		if not p.has_method("get_audio_instrumentation"):
			continue
		var inst = p.get_audio_instrumentation()
		var playing: bool = p.has_method("is_playing") and p.is_playing()
		var info := _classify_player_instrumentation(p, inst, playing)
		var bkey: String = str(info.bucket)
		if bkey == "idle":
			idle_n += 1
		elif bkey == "running_no_data":
			run_no_data += 1
			problem_details.append(info)
		elif bkey == "running_issue":
			run_issue += 1
			problem_details.append(info)
		elif bkey == "running_ok":
			run_ok += 1
			if int(info.get("ui_severity", 0)) == 1:
				run_watch += 1
				watch_details.append(info)

	var total_n := idle_n + run_no_data + run_issue + run_ok
	var parts: PackedStringArray = []
	var bs_txt := (
		"[color=%s]%s[/color]"
		% [
			COLOR_NEUTRAL,
			"block=%d" % block_size if block_size > 0 else "block=?",
		]
	)
	var c_nd := COLOR_WARNING if run_no_data > 0 else COLOR_NEUTRAL
	var c_iss := COLOR_ERROR if run_issue > 0 else COLOR_NEUTRAL
	(
		parts
		. append(
			(
				"[color=%s]Sources:[/color] %d total | idle %d | [color=%s]playing OK %d[/color] | [color=%s]playing no data %d[/color] | [color=%s]playing issues %d[/color] | %s"
				% [
					COLOR_NEUTRAL,
					total_n,
					idle_n,
					COLOR_OK,
					run_ok,
					c_nd,
					run_no_data,
					c_iss,
					run_issue,
					bs_txt
				]
			)
		)
	)

	if _audio_show_details:
		if not problem_details.is_empty():
			parts.append(
				(
					"[color=%s]- issues / no data (max %d) -[/color]"
					% [COLOR_HINT, AUDIO_PROBLEM_DETAIL_CAP]
				)
			)
			var n := mini(AUDIO_PROBLEM_DETAIL_CAP, problem_details.size())
			for j in range(n):
				var d: Dictionary = problem_details[j]
				for line in d.detail:
					parts.append(line)
		if not watch_details.is_empty():
			parts.append(
				"[color=%s]- watch: elevated late-mix rate (usually benign) -[/color]" % COLOR_HINT
			)
			var nw := mini(AUDIO_PROBLEM_DETAIL_CAP, watch_details.size())
			for j in range(nw):
				var w: Dictionary = watch_details[j]
				for line in w.detail:
					parts.append(line)
	elif run_no_data > 0 or run_issue > 0 or run_watch > 0:
		parts.append("[color=%s]Alt+A[/color] per-source details (issues, then watch)" % COLOR_HINT)

	_audio_instrumentation_label.text = "\n".join(parts)


func _refresh_reverb_bus() -> void:
	if not _reverb_compact_label:
		return
	var srv: Variant = ResonanceServerAccess.get_server()
	if not srv or not srv.has_method("get_reverb_bus_instrumentation"):
		_reverb_compact_label.text = "[color=%s]ResonanceServer not available[/color]" % COLOR_ERROR
		return

	var ri: Dictionary = srv.get_reverb_bus_instrumentation()
	var refl_type: int = ri.get("reflection_type", -1)
	var mixer_ok: bool = ri.get("mixer_exists", false)
	var feeds: int = ri.get("mixer_feed_count", 0)

	var eff_proc: int = ri.get("effect_process_calls", 0)
	var eff_ok: int = ri.get("effect_success", 0)
	var eff_null: int = ri.get("effect_mixer_null", 0)
	# Convolution/TAN use ReflectionMixer on the bus. Parametric/Hybrid have no mixer - wet signal is mixed inside ResonancePlayer.
	var parametric_or_hybrid := refl_type == 1 or refl_type == 2
	var eff_col := COLOR_OK if eff_ok > 0 and eff_null == 0 else COLOR_WARNING
	if parametric_or_hybrid and eff_null > 0 and eff_ok == 0:
		eff_col = COLOR_NEUTRAL

	var fetch_hit := int(ri.get("fetch_cache_hit", 0))
	var fetch_miss := int(ri.get("fetch_cache_miss", 0))
	var fetch_skip := int(ri.get("fetch_cache_skip", 0))
	var fetch_total := fetch_hit + fetch_miss
	var miss_pct := (100.0 * fetch_miss / fetch_total) if fetch_total > 0 else 0.0
	var miss_col := (
		COLOR_OK if fetch_miss == 0 else (COLOR_WARNING if miss_pct < 10.0 else COLOR_ERROR)
	)

	var compact: PackedStringArray = []
	var mixer_lbl := "ok" if mixer_ok else "missing"
	var mixer_col := COLOR_OK if mixer_ok else COLOR_WARNING
	if parametric_or_hybrid and not mixer_ok:
		mixer_lbl = "n/a (player output)"
		mixer_col = COLOR_NEUTRAL
	compact.append(
		(
			"[color=%s]Mixer:[/color] [color=%s]%s[/color]  [color=%s]feeds=%d[/color]"
			% [COLOR_NEUTRAL, mixer_col, mixer_lbl, COLOR_NEUTRAL, feeds]
		)
	)
	compact.append(
		(
			"[color=%s]Effect:[/color] [color=%s]success %d / %d[/color] (mixer_null=%d) peak=%.4f"
			% [
				COLOR_NEUTRAL,
				eff_col,
				eff_ok,
				eff_proc,
				eff_null,
				ri.get("effect_output_peak", 0.0)
			]
		)
	)
	if parametric_or_hybrid:
		(
			compact
			. append(
				(
					"[color=%s]Bus effect:[/color] silent by design - use [color=%s]ResonancePlayer[/color] RMS / signal levels for wet audio."
					% [COLOR_HINT, COLOR_NEUTRAL]
				)
			)
		)
	compact.append(
		(
			"[color=%s]Fetch reverb:[/color] [color=%s]miss %.1f%%[/color] (hit=%d miss=%d skip=%d)"
			% [COLOR_NEUTRAL, miss_col, miss_pct, fetch_hit, fetch_miss, fetch_skip]
		)
	)
	if srv.has_method("get_pathing_instrumentation"):
		var pi: Dictionary = srv.get_pathing_instrumentation()
		var sim_attempt: int = int(pi.get("sim_attempt", 0))
		var sim_ran: int = int(pi.get("sim_ran", 0))
		var sim_seh: int = int(pi.get("sim_seh_fail", 0))
		var skip_l: int = int(pi.get("sim_skip_listener", 0))
		var skip_cd: int = int(pi.get("sim_skip_cooldown", 0))
		var sh_ok: int = int(pi.get("fetch_sh_ok", 0))
		var sh_null: int = int(pi.get("fetch_sh_null", 0))
		var p_miss: int = int(pi.get("player_fetch_miss", 0))
		var p_ap: int = int(pi.get("player_applied", 0))
		var p_gate: int = int(pi.get("player_gate", 0))
		var path_col := (
			COLOR_OK
			if sim_ran > 0 and sh_ok > 0 and p_miss == 0
			else (COLOR_WARNING if sim_ran > 0 or sh_ok > 0 else COLOR_NEUTRAL)
		)
		if sim_attempt > 0 and sim_ran == 0:
			path_col = COLOR_ERROR
		if sim_seh > 0 and sim_ran == 0:
			path_col = COLOR_ERROR
		if p_miss > 0 and sh_null > 0:
			path_col = COLOR_ERROR
		(
			compact
			. append(
				(
					"[color=%s]Pathing:[/color] [color=%s]attempt=%d ran=%d seh_fail=%d[/color] skip_L=%d skip_cd=%d | sh_ok=%d sh_null=%d | player gate=%d applied=%d miss=%d"
					% [
						COLOR_NEUTRAL,
						path_col,
						sim_attempt,
						sim_ran,
						sim_seh,
						skip_l,
						skip_cd,
						sh_ok,
						sh_null,
						p_gate,
						p_ap,
						p_miss
					]
				)
			)
		)

	var runtimes := get_tree().get_nodes_in_group("resonance_runtime")
	if not runtimes.is_empty():
		var rt: Node = runtimes[0]
		if rt.has_method("get_bus_effective"):
			compact.append(
				"[color=%s]Output bus:[/color] %s" % [COLOR_NEUTRAL, rt.get_bus_effective()]
			)
		var ai = rt.get("activator_instrumentation")
		if ai != null and not ai.is_empty():
			var act: bool = ai.get("active", false)
			var acol := COLOR_OK if act else COLOR_WARNING
			(
				compact
				. append(
					(
						"[color=%s]Activator:[/color] [color=%s]%s[/color] frames=%d skips=%s"
						% [
							COLOR_NEUTRAL,
							acol,
							"active" if act else str(ai.get("reason", "?")),
							ai.get("frames_pushed_total", 0),
							str(ai.get("skips", "?")),
						]
					)
				)
			)

	_reverb_compact_label.text = "\n".join(compact)


func _str_bool(b: bool) -> String:
	return "ON" if b else "OFF"

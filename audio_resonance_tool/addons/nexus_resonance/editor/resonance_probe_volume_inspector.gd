@tool
extends EditorInspectorPlugin

## ResonanceProbeVolume inspector: bake / preview / probe batch edits. [member bake_runner] is set by the plugin.

const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")
const ResonanceEditorDialogs = preload(
	"res://addons/nexus_resonance/editor/resonance_editor_dialogs.gd"
)

var bake_runner = null
var editor_interface: EditorInterface = null


func _can_handle(object: Object) -> bool:
	return object != null and object.is_class("ResonanceProbeVolume")


func _parse_begin(object: Object) -> void:
	if not editor_interface:
		return

	var base: Control = editor_interface.get_base_control() if editor_interface else null

	# Preview Bake Settings
	var preview_btn = Button.new()
	preview_btn.text = tr(UIStrings.BTN_PREVIEW_BAKE_SETTINGS)
	preview_btn.tooltip_text = tr(UIStrings.TT_PREVIEW_BAKE)
	preview_btn.pressed.connect(_on_preview_bake_pressed.bind(object))
	add_custom_control(preview_btn)

	# Bake Probes
	var btn = Button.new()
	btn.text = tr(UIStrings.BTN_BAKE_PROBES)
	btn.tooltip_text = tr(UIStrings.TT_BAKE_PROBES)
	btn.icon = ResonanceEditorDialogs.get_icon(base, UIStrings.ICON_BAKE, "Bake")
	btn.pressed.connect(_on_bake_pressed.bind(object))
	add_custom_control(btn)

	_add_probe_batch_edit_section(object)


func _add_probe_batch_edit_section(vol: Object) -> void:
	if not vol.has_method("get_probe_data"):
		return
	var pd = vol.get_probe_data()
	if pd == null:
		return
	var has_data: bool = not pd.get_data().is_empty()
	var srv = ResonanceServerAccess.get_server()
	var server_ready: bool = srv != null and srv.is_initialized()
	var fold := FoldableContainer.new()
	fold.title = tr(UIStrings.INSPECTOR_PROBE_BATCH_EDIT)
	fold.folded = true
	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 6)
	if not server_ready:
		var need_srv := Label.new()
		need_srv.text = tr(UIStrings.INSPECTOR_PROBE_BATCH_SERVER_REQUIRED)
		need_srv.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		need_srv.add_theme_color_override("font_color", Color(1.0, 0.55, 0.35))
		vbox.add_child(need_srv)
	var num_probes := -1
	if srv and srv.is_initialized() and srv.has_method("editor_probe_data_get_num_probes"):
		num_probes = int(srv.editor_probe_data_get_num_probes(pd))
	if num_probes < 0 and pd.get_probe_positions().size() > 0:
		num_probes = pd.get_probe_positions().size()
	var count_lbl := Label.new()
	count_lbl.text = (
		tr(UIStrings.INSPECTOR_PROBE_COUNT_KNOWN) % int(num_probes)
		if num_probes >= 0
		else tr(UIStrings.INSPECTOR_PROBE_COUNT_UNKNOWN)
	)
	count_lbl.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	vbox.add_child(count_lbl)
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	var spin := SpinBox.new()
	spin.min_value = 0
	spin.max_value = maxi(0, num_probes - 1) if num_probes > 0 else 0
	spin.step = 1
	spin.rounded = true
	spin.editable = has_data and server_ready
	var rm_btn := Button.new()
	rm_btn.text = tr(UIStrings.BTN_REMOVE_PROBE_AT_INDEX)
	rm_btn.tooltip_text = tr(UIStrings.TT_REMOVE_PROBE)
	rm_btn.disabled = not has_data or not server_ready
	var batch_ui := {
		"count_lbl": count_lbl, "spin": spin, "rm_btn": rm_btn, "path_btn": null, "refl_btn": null
	}
	rm_btn.pressed.connect(_on_remove_probe_at_index_pressed.bind(vol, spin, batch_ui))
	row.add_child(spin)
	row.add_child(rm_btn)
	vbox.add_child(row)
	var path_btn := Button.new()
	path_btn.text = tr(UIStrings.BTN_REMOVE_BAKED_PATHING)
	path_btn.tooltip_text = tr(UIStrings.TT_REMOVE_PATHING)
	path_btn.disabled = not has_data or not server_ready
	batch_ui["path_btn"] = path_btn
	path_btn.pressed.connect(_on_remove_baked_pathing_pressed.bind(vol, batch_ui))
	vbox.add_child(path_btn)
	var refl_btn := Button.new()
	refl_btn.text = tr(UIStrings.BTN_REMOVE_BAKED_REFLECTION_REVERB)
	refl_btn.tooltip_text = tr(UIStrings.TT_REMOVE_REFLECTION_REVERB)
	refl_btn.disabled = not has_data or not server_ready
	batch_ui["refl_btn"] = refl_btn
	refl_btn.pressed.connect(_on_remove_baked_reflection_reverb_pressed.bind(vol, batch_ui))
	vbox.add_child(refl_btn)
	fold.add_child(vbox)
	add_custom_control(fold)


func _refresh_probe_batch_section_ui(vol: Object, batch_ui: Dictionary) -> void:
	var count_lbl: Variant = batch_ui.get("count_lbl", null)
	var spin: Variant = batch_ui.get("spin", null)
	if vol == null or count_lbl == null or spin == null:
		return
	var pd: Variant = vol.get_probe_data() if vol.has_method("get_probe_data") else null
	var has_data: bool = pd != null and not pd.get_data().is_empty()
	var srv: Variant = ResonanceServerAccess.get_server()
	var server_ready: bool = srv != null and srv.is_initialized()
	var num_probes := -1
	if srv and srv.is_initialized() and srv.has_method("editor_probe_data_get_num_probes") and pd:
		num_probes = int(srv.editor_probe_data_get_num_probes(pd))
	if num_probes < 0 and pd and pd.get_probe_positions().size() > 0:
		num_probes = pd.get_probe_positions().size()
	if count_lbl is Label:
		(count_lbl as Label).text = (
			tr(UIStrings.INSPECTOR_PROBE_COUNT_KNOWN) % int(num_probes)
			if num_probes >= 0
			else tr(UIStrings.INSPECTOR_PROBE_COUNT_UNKNOWN)
		)
	if spin is SpinBox:
		var sb := spin as SpinBox
		var mx := maxi(0, num_probes - 1) if num_probes > 0 else 0
		sb.max_value = mx
		if sb.value > mx:
			sb.value = mx
		sb.editable = has_data and server_ready
	for key in ["rm_btn", "path_btn", "refl_btn"]:
		var b: Variant = batch_ui.get(key, null)
		if b is Button:
			(b as Button).disabled = not has_data or not server_ready


func _probe_edit_post_success(vol: Object, pd: Resource, batch_ui: Dictionary = {}) -> void:
	pd.emit_changed()
	if not pd.resource_path.is_empty():
		ResourceSaver.save(pd, pd.resource_path)
	if editor_interface:
		editor_interface.mark_scene_as_unsaved()
	if not batch_ui.is_empty():
		_refresh_probe_batch_section_ui(vol, batch_ui)
	# Deferred: let save/mark unsaved settle before native reload (avoids no-op).
	if vol.has_method("reload_probe_batch"):
		vol.call_deferred("reload_probe_batch")


func _warn_probe_server_required() -> void:
	ResonanceEditorDialogs.show_warning(editor_interface, tr(UIStrings.WARN_PROBE_EDIT_NO_SERVER))


func _warn_probe_edit_failed() -> void:
	ResonanceEditorDialogs.show_warning(editor_interface, tr(UIStrings.WARN_PROBE_EDIT_FAILED))


func _require_server_method(method_name: StringName) -> Variant:
	var srv = ResonanceServerAccess.get_server()
	if srv == null or not srv.is_initialized() or not srv.has_method(method_name):
		_warn_probe_server_required()
		return null
	return srv


func _ensure_server_for_probe_edit(vol: Object) -> bool:
	if (
		bake_runner
		and vol is Node
		and bake_runner.has_method("ensure_resonance_server_for_volumes")
	):
		var vol_nodes: Array[Node] = []
		vol_nodes.append(vol as Node)
		return bake_runner.ensure_resonance_server_for_volumes(vol_nodes)
	var srv = ResonanceServerAccess.get_server()
	return srv != null and srv.is_initialized()


func _on_remove_probe_at_index_pressed(vol: Object, spin: SpinBox, batch_ui: Dictionary) -> void:
	var pd = vol.get_probe_data()
	if pd == null or pd.get_data().is_empty():
		return
	if not _ensure_server_for_probe_edit(vol):
		_warn_probe_server_required()
		return
	var srv := _require_server_method(&"editor_probe_data_remove_probe")
	if srv == null:
		return
	var idx := int(spin.value)
	if not srv.editor_probe_data_remove_probe(pd, idx):
		_warn_probe_edit_failed()
		return
	_probe_edit_post_success(vol, pd, batch_ui)


func _on_remove_baked_pathing_pressed(vol: Object, batch_ui: Dictionary) -> void:
	var pd = vol.get_probe_data()
	if pd == null or pd.get_data().is_empty():
		return
	if not _ensure_server_for_probe_edit(vol):
		_warn_probe_server_required()
		return
	var srv := _require_server_method(&"editor_probe_data_remove_baked_layer")
	if srv == null:
		return
	# Type 1 = pathing, variation 3 = dynamic (same as bake_pathing).
	if not srv.editor_probe_data_remove_baked_layer(pd, 1, 3, Vector3.ZERO, 0.0):
		_warn_probe_edit_failed()
		return
	_probe_edit_post_success(vol, pd, batch_ui)


func _on_remove_baked_reflection_reverb_pressed(vol: Object, batch_ui: Dictionary) -> void:
	var pd = vol.get_probe_data()
	if pd == null or pd.get_data().is_empty():
		return
	if not _ensure_server_for_probe_edit(vol):
		_warn_probe_server_required()
		return
	var srv := _require_server_method(&"editor_probe_data_remove_baked_layer")
	if srv == null:
		return
	# Type 0 = reflections/reverb, variation 0 = reverb IR.
	if not srv.editor_probe_data_remove_baked_layer(pd, 0, 0, Vector3.ZERO, 0.0):
		_warn_probe_edit_failed()
		return
	_probe_edit_post_success(vol, pd, batch_ui)


func _on_preview_bake_pressed(obj: Object) -> void:
	if not obj or not obj.is_class("ResonanceProbeVolume"):
		return
	var probe_count = (
		bake_runner.estimate_probe_count(obj)
		if bake_runner and bake_runner.has_method("estimate_probe_count")
		else -1
	)
	var est_time = (
		bake_runner.estimate_bake_time(obj)
		if bake_runner and bake_runner.has_method("estimate_bake_time")
		else ""
	)
	var msg := ""
	if probe_count >= 0:
		msg = tr(UIStrings.PROGRESS_PROBES) % probe_count
	if not est_time.is_empty():
		msg = (
			msg + "\n" + tr(UIStrings.PROGRESS_ESTIMATED_TIME) % est_time
			if not msg.is_empty()
			else tr(UIStrings.PROGRESS_ESTIMATED_TIME) % est_time
		)
	if msg.is_empty():
		msg = tr(UIStrings.INFO_CONFIGURE_BAKE_CONFIG_FOR_ESTIMATES)
	if editor_interface:
		ResonanceEditorDialogs.show_success_toast(editor_interface, msg)
	else:
		print_rich("[color=cyan]Nexus Resonance:[/color] " + msg)


func _on_bake_pressed(obj: Object) -> void:
	if not obj or not obj.is_class("ResonanceProbeVolume"):
		return
	if bake_runner:
		var volumes: Array[Node] = []
		volumes.append(obj)
		if bake_runner.has_method("ensure_resonance_server_for_volumes"):
			if not bake_runner.ensure_resonance_server_for_volumes(volumes):
				return
		bake_runner.run_bake(volumes)
	else:
		ResonanceEditorDialogs.show_warning(
			editor_interface, tr(UIStrings.WARN_BAKE_RUNNER_NOT_SET)
		)

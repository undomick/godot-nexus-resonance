@tool
extends EditorInspectorPlugin

## ResonanceReverbDataPoint inspector: sample baked reverb and show RT60 / energy.

const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")
const QueryUI = preload(
	"res://addons/nexus_resonance/scripts/resonance_reverb_data_point_query_ui.gd"
)

var bake_runner = null
var editor_interface: EditorInterface = null
var _result_box: VBoxContainer = null


func _can_handle(object: Object) -> bool:
	return object != null and object.is_class("ResonanceReverbDataPoint")


func _parse_begin(object: Object) -> void:
	var btn := Button.new()
	btn.text = tr(UIStrings.BTN_SAMPLE_REVERB_HERE)
	btn.tooltip_text = tr(UIStrings.TT_SAMPLE_REVERB_HERE)
	btn.pressed.connect(_on_query_pressed.bind(object))
	add_custom_control(btn)

	_result_box = VBoxContainer.new()
	add_custom_control(_result_box)
	_refresh_result_labels(object)


func _on_query_pressed(obj: Object) -> void:
	if obj == null or not obj.is_class("ResonanceReverbDataPoint"):
		return
	_ensure_probe_data_from_ancestor(obj)
	if obj.get("probe_data") == null:
		push_warning(UIStrings.PREFIX + tr(UIStrings.WARN_REVERB_QUERY_NO_PROBE_DATA))
		_refresh_result_labels(obj)
		return

	if bake_runner and bake_runner.has_method("ensure_resonance_server_for_volumes"):
		var volumes: Array[Node] = []
		var vol: Node = QueryUI.find_ancestor_probe_volume(obj as Node)
		if vol != null:
			volumes.append(vol)
		if not bake_runner.ensure_resonance_server_for_volumes(volumes):
			_refresh_result_labels(obj)
			return
	elif bake_runner == null:
		push_warning(UIStrings.PREFIX + tr(UIStrings.WARN_BAKE_RUNNER_NOT_SET))

	if obj.has_method("query_baked_reverb"):
		obj.call("query_baked_reverb")
	if obj is Node3D:
		(obj as Node3D).update_gizmos()
	if obj is Object and obj.has_method("notify_property_list_changed"):
		obj.notify_property_list_changed()
	_refresh_result_labels(obj)


func _ensure_probe_data_from_ancestor(obj: Object) -> void:
	if obj.get("probe_data") != null:
		return
	var pd = QueryUI.find_ancestor_probe_data(obj as Node)
	if pd == null:
		return
	obj.set("probe_data", pd)
	if editor_interface:
		EditorInterface.mark_scene_as_unsaved()


func _refresh_result_labels(obj: Object) -> void:
	if _result_box == null:
		return
	for c in _result_box.get_children():
		c.queue_free()

	var query: Variant = obj.get("last_query") if obj else null
	var mix_rate := 48000.0
	if Engine.has_singleton("AudioServer"):
		mix_rate = float(AudioServer.get_mix_rate())
	for line in QueryUI.format_query_lines(query, mix_rate):
		_add_result_line(line)


func _add_result_line(text: String) -> void:
	var lbl := Label.new()
	lbl.text = text
	lbl.add_theme_font_size_override("font_size", 11)
	_result_box.add_child(lbl)

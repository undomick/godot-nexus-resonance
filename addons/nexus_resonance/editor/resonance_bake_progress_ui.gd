@tool
extends RefCounted
class_name ResonanceBakeProgressUI

## Bake progress dialog, stage label, and details log. Used by the bake pipeline.

const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")

var editor_interface: EditorInterface
var cancel_requested: bool = false

var _progress_dialog: AcceptDialog = null
var _progress_bar: ProgressBar = null
var _stage_label: Label = null
var _status_label: Label = null
var _details_panel: TextEdit = null
var _bake_start_time: float = 0.0


func _init(p_editor_interface: EditorInterface) -> void:
	editor_interface = p_editor_interface


func show_ui() -> void:
	if not editor_interface:
		return
	cancel_requested = false
	var base: Control = editor_interface.get_base_control()
	if not base:
		return
	_progress_dialog = AcceptDialog.new()
	_progress_dialog.title = UIStrings.DIALOG_PROGRESS_TITLE
	_bake_start_time = Time.get_ticks_msec() / 1000.0
	_progress_dialog.theme = editor_interface.get_editor_theme()
	_progress_dialog.dialog_hide_on_ok = false
	_progress_dialog.get_ok_button().visible = false
	_progress_dialog.exclusive = false

	var vbox = VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 12)
	_progress_dialog.add_child(vbox)

	var status_section = VBoxContainer.new()
	status_section.add_theme_constant_override("separation", 4)
	var status_hdr = Label.new()
	status_hdr.text = UIStrings.PROGRESS_STATUS
	status_hdr.add_theme_font_size_override("font_size", 11)
	status_section.add_child(status_hdr)
	_stage_label = Label.new()
	_stage_label.name = "StageLabel"
	_stage_label.text = ""
	_stage_label.add_theme_font_size_override("font_size", 10)
	status_section.add_child(_stage_label)
	_status_label = Label.new()
	_status_label.name = "StatusLabel"
	_status_label.text = UIStrings.PROGRESS_PREPARING
	_status_label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	_status_label.add_theme_font_size_override("font_size", 16)
	status_section.add_child(_status_label)
	vbox.add_child(status_section)

	vbox.add_child(HSeparator.new())

	var progress_section = VBoxContainer.new()
	progress_section.add_theme_constant_override("separation", 4)
	_progress_bar = ProgressBar.new()
	_progress_bar.custom_minimum_size = Vector2(320, 28)
	_progress_bar.show_percentage = true
	_progress_bar.min_value = 0.0
	_progress_bar.max_value = 1.0
	_progress_bar.value = 0.0
	progress_section.add_child(_progress_bar)
	vbox.add_child(progress_section)

	var details_hdr = Label.new()
	details_hdr.text = UIStrings.PROGRESS_DETAILS
	details_hdr.add_theme_font_size_override("font_size", 10)
	vbox.add_child(details_hdr)
	_details_panel = TextEdit.new()
	_details_panel.custom_minimum_size = Vector2(380, 80)
	_details_panel.editable = false
	_details_panel.wrap_mode = TextEdit.LINE_WRAPPING_BOUNDARY
	vbox.add_child(_details_panel)

	var cancel_btn = Button.new()
	cancel_btn.text = UIStrings.BTN_CANCEL
	cancel_btn.tooltip_text = UIStrings.TT_CANCEL_BAKE
	cancel_btn.pressed.connect(_on_cancel_pressed)
	vbox.add_child(cancel_btn)
	_progress_dialog.close_requested.connect(_on_cancel_pressed)
	base.add_child(_progress_dialog)
	_progress_dialog.popup_centered(Vector2i(440, 320))
	Callable(cancel_btn, "grab_focus").call_deferred()


func hide_ui() -> void:
	if _progress_dialog == null:
		_progress_bar = null
		_stage_label = null
		_status_label = null
		_details_panel = null
		return
	if (
		_progress_dialog.is_inside_tree()
		and _progress_dialog.close_requested.is_connected(_on_cancel_pressed)
	):
		_progress_dialog.close_requested.disconnect(_on_cancel_pressed)
	_progress_dialog.queue_free()
	_progress_dialog = null
	_progress_bar = null
	_stage_label = null
	_status_label = null
	_details_panel = null


func shutdown() -> void:
	# Disconnect signals and free the dialog so shutdown cannot leak nodes or connections.
	hide_ui()
	editor_interface = null


func poll_server_progress() -> void:
	var srv: Variant = ResonanceServerAccess.get_server()
	if srv == null or _progress_bar == null:
		return
	if srv.has_method("get_bake_progress"):
		_progress_bar.value = float(srv.get_bake_progress())


func set_bake_status(text: String) -> void:
	if _status_label:
		_status_label.call_deferred("set_text", text)
	if _details_panel:
		_append_details(text)


func set_stage(current: int, total: int, est_remaining: String = "") -> void:
	if _stage_label:
		var t: String = UIStrings.PROGRESS_STAGE % [current, total]
		if not est_remaining.is_empty():
			t += "  " + est_remaining
		_stage_label.call_deferred("set_text", t)


func clear_details() -> void:
	if _details_panel:
		_details_panel.call_deferred("set_text", "")


func _append_details(line: String) -> void:
	if _details_panel:
		var elapsed = (Time.get_ticks_msec() / 1000.0) - _bake_start_time
		_details_panel.call_deferred(
			"set_text", _details_panel.text + "[%.1fs] %s\n" % [elapsed, line]
		)


func _on_cancel_pressed() -> void:
	cancel_requested = true
	var srv: Variant = ResonanceServerAccess.get_server()
	if srv:
		srv.cancel_reflections_bake()
		srv.cancel_pathing_bake()

@tool
extends RefCounted
class_name ResonanceEditorJobProgress

## Generic editor progress dialog (export / project scan). Same cancel pattern as bake UI.

const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")

var editor_interface: EditorInterface
var cancel_requested: bool = false

var _dialog: AcceptDialog = null
var _progress_bar: ProgressBar = null
var _stage_label: Label = null
var _status_label: Label = null


func _init(p_editor_interface: EditorInterface) -> void:
	editor_interface = p_editor_interface


func show_job(title: String, total_steps: int) -> void:
	if not editor_interface:
		return
	hide_job()
	cancel_requested = false
	var base: Control = editor_interface.get_base_control()
	if not base:
		return
	_dialog = AcceptDialog.new()
	_dialog.title = title
	_dialog.theme = editor_interface.get_editor_theme()
	_dialog.dialog_hide_on_ok = false
	_dialog.get_ok_button().visible = false
	_dialog.exclusive = false

	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 10)
	_dialog.add_child(vbox)

	_stage_label = Label.new()
	_stage_label.text = UIStrings.PROGRESS_STAGE % [0, maxi(total_steps, 1)]
	_stage_label.add_theme_font_size_override("font_size", 11)
	vbox.add_child(_stage_label)

	_status_label = Label.new()
	_status_label.text = UIStrings.PROGRESS_PREPARING
	_status_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	vbox.add_child(_status_label)

	_progress_bar = ProgressBar.new()
	_progress_bar.custom_minimum_size = Vector2(360, 24)
	_progress_bar.min_value = 0.0
	_progress_bar.max_value = 1.0
	_progress_bar.value = 0.0
	vbox.add_child(_progress_bar)

	var cancel_btn := Button.new()
	cancel_btn.text = UIStrings.BTN_CANCEL
	cancel_btn.pressed.connect(_on_cancel_pressed)
	vbox.add_child(cancel_btn)
	_dialog.close_requested.connect(_on_cancel_pressed)

	base.add_child(_dialog)
	_dialog.popup_centered(Vector2i(420, 200))


func set_step(current: int, total: int, status_text: String = "") -> void:
	if _stage_label:
		_stage_label.text = UIStrings.PROGRESS_STAGE % [current, maxi(total, 1)]
	if not status_text.is_empty() and _status_label:
		_status_label.text = status_text
	if _progress_bar and total > 0:
		_progress_bar.value = float(current) / float(total)


func hide_job() -> void:
	if _dialog:
		if _dialog.close_requested.is_connected(_on_cancel_pressed):
			_dialog.close_requested.disconnect(_on_cancel_pressed)
		_dialog.queue_free()
	_dialog = null
	_progress_bar = null
	_stage_label = null
	_status_label = null


func shutdown() -> void:
	hide_job()
	editor_interface = null


func _on_cancel_pressed() -> void:
	cancel_requested = true

@tool
extends RefCounted
class_name ResonanceEditorDialogs

## Dialogs, toasts, and checklist rows for the addon (consistent with editor theme).

const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")


## Checklist row: icon + label. [param icon_size] 16 default, 14 for compact inspector; [param row_separation] icon–text gap.
static func create_checklist_row(
	base: Control, label: String, ok: bool, icon_size: int = 16, row_separation: int = 8
) -> HBoxContainer:
	var row = HBoxContainer.new()
	row.add_theme_constant_override("separation", row_separation)
	var icon_name := "StatusSuccess" if ok else "StatusError"
	var icon_texture = base.get_theme_icon(icon_name, "EditorIcons")
	if icon_texture:
		var icon_rect = TextureRect.new()
		icon_rect.texture = icon_texture
		icon_rect.custom_minimum_size = Vector2(icon_size, icon_size)
		icon_rect.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
		row.add_child(icon_rect)
	var lbl = Label.new()
	lbl.text = label
	if icon_size == 14:
		lbl.add_theme_font_size_override("font_size", 12)
	var ok_col := Color(0.4, 0.9, 0.4)
	var err_col := Color(1.0, 0.5, 0.5)
	lbl.add_theme_color_override("font_color", ok_col if ok else err_col)
	row.add_child(lbl)
	return row


## Icon from [param icon_path], else theme icon [param fallback_icon]. Null if [param base] is null.
static func get_icon(base: Control, icon_path: String, fallback_icon: String) -> Texture2D:
	if not base:
		return null
	var icon = load(icon_path) as Texture2D if ResourceLoader.exists(icon_path) else null
	if icon:
		return icon
	return base.get_theme_icon(fallback_icon, "EditorIcons")


static func _speak(message: String) -> void:
	if ProjectSettings.has_setting("nexus/nexus_resonance/accessibility/editor_tts") and ProjectSettings.get_setting("nexus/nexus_resonance/accessibility/editor_tts"):
		var voice_index := 0
		if ProjectSettings.has_setting("nexus/nexus_resonance/accessibility/tts_voice"):
			voice_index = int(ProjectSettings.get_setting("nexus/nexus_resonance/accessibility/tts_voice"))
		
		var voices := DisplayServer.tts_get_voices()
		var voice_id := ""
		if voice_index > 0 and voice_index - 1 < voices.size():
			voice_id = voices[voice_index - 1]["id"]
		elif not voices.is_empty():
			voice_id = voices[0]["id"]
		
		var volume := 50
		if ProjectSettings.has_setting("nexus/nexus_resonance/accessibility/tts_volume"):
			volume = int(ProjectSettings.get_setting("nexus/nexus_resonance/accessibility/tts_volume"))
			
		var speed := 1.0
		if ProjectSettings.has_setting("nexus/nexus_resonance/accessibility/tts_speed"):
			speed = float(ProjectSettings.get_setting("nexus/nexus_resonance/accessibility/tts_speed"))
			
		DisplayServer.tts_speak(message, voice_id, volume, 1.0, speed)


## Modal error (blocking).
static func show_critical(
	editor_interface: EditorInterface, message: String, title: String = ""
) -> void:
	var t = title if not title.is_empty() else UIStrings.DIALOG_BAKE_FAILED_TITLE
	_speak(t + ": " + message)
	show_error_dialog(editor_interface, t, message)


## Non-blocking warning (EditorToaster when available).
static func show_warning(editor_interface: EditorInterface, message: String) -> void:
	_speak(message)
	if _try_push_toast(editor_interface, message, 1):
		return
	push_warning(UIStrings.PREFIX + message)


## Prints colored line (no dialog).
static func show_info(message: String) -> void:
	_speak(message)
	print_rich("[color=cyan]Nexus Resonance:[/color] " + message)


## Error dialog; optional cause, solution, doc link button.
static func show_error_dialog(
	editor_interface: EditorInterface,
	title: String,
	message: String,
	cause: String = "",
	solution: String = "",
	doc_link: String = ""
) -> void:
	if not editor_interface:
		push_error(UIStrings.PREFIX + title + " - " + message)
		return
	var base = editor_interface.get_base_control()
	if not base:
		push_error(UIStrings.PREFIX + title + " - " + message)
		return
	var dialog = AcceptDialog.new()
	dialog.title = title
	var full_text := message
	if not cause.is_empty():
		full_text += "\n\nCause: " + cause
	if not solution.is_empty():
		full_text += "\n\nSolution: " + solution
	dialog.dialog_text = full_text
	dialog.theme = editor_interface.get_editor_theme()
	dialog.min_size = Vector2i(420, 0)
	dialog.confirmed.connect(dialog.queue_free)
	dialog.close_requested.connect(dialog.queue_free)
	base.add_child(dialog)
	if not doc_link.is_empty():
		var vbox = dialog.get_child(0)
		if vbox is VBoxContainer:
			var link_btn = LinkButton.new()
			link_btn.text = UIStrings.BTN_DOCUMENTATION
			link_btn.uri = doc_link
			vbox.add_child(link_btn)
	dialog.popup_centered()


## Success: toaster if available, else small AcceptDialog.
static func show_success_toast(editor_interface: EditorInterface, message: String) -> void:
	_speak(message)
	if not editor_interface:
		show_info(message)
		return
	if _try_push_toast(editor_interface, message, 0):
		return
	_fallback_success_dialog(editor_interface, message)


static func _fallback_success_dialog(editor_interface: EditorInterface, message: String) -> void:
	var base = editor_interface.get_base_control()
	if not base:
		show_info(message)
		return
	var dialog = AcceptDialog.new()
	dialog.title = UIStrings.ADDON_NAME
	dialog.dialog_text = message
	dialog.theme = editor_interface.get_editor_theme()
	dialog.confirmed.connect(dialog.queue_free)
	dialog.close_requested.connect(dialog.queue_free)
	base.add_child(dialog)
	dialog.popup_centered()
	dialog.get_ok_button().call_deferred("grab_focus")


static func _try_push_toast(
	editor_interface: EditorInterface, message: String, severity: int
) -> bool:
	if not editor_interface or not editor_interface.has_method("get_editor_toaster"):
		return false
	var toaster = editor_interface.get_editor_toaster()
	if toaster and toaster.has_method("push_toast"):
		toaster.push_toast(message, severity, "")
		return true
	return false


## Bake validation checklist: [code]{ "label", "ok" }[/code]. [param on_confirmed] receives [param all_ok].
## Optional export link when [param show_export_link] and [param on_export_static] are set.
static func show_validation_dialog(
	editor_interface: EditorInterface,
	title: String,
	checklist: Array,
	on_confirmed: Callable = Callable(),
	show_export_link: bool = false,
	on_export_static: Callable = Callable()
) -> void:
	if not editor_interface:
		return
	var base = editor_interface.get_base_control()
	if not base:
		return
	var dialog = ConfirmationDialog.new()
	dialog.title = title
	dialog.theme = editor_interface.get_editor_theme()
	dialog.min_size = Vector2i(400, 0)
	var vbox = VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 6)
	var all_ok := true
	for item in checklist:
		var label: String = str(item.get("label", ""))
		var ok: bool = item.get("ok", false)
		if not ok:
			all_ok = false
		vbox.add_child(create_checklist_row(base, label, ok, 16, 8))
	if show_export_link and on_export_static.is_valid():
		var link_btn = LinkButton.new()
		link_btn.text = "Export Static Scene (Ctrl+Shift+E)"
		link_btn.pressed.connect(
			func():
				dialog.queue_free()
				on_export_static.call()
		)
		vbox.add_child(link_btn)
	dialog.add_child(vbox)
	dialog.dialog_text = ""
	dialog.ok_button_text = UIStrings.BTN_CONTINUE if all_ok else UIStrings.BTN_BAKE_ANYWAY
	dialog.cancel_button_text = UIStrings.BTN_CANCEL
	dialog.get_label().visible = false
	var cleanup = func():
		dialog.queue_free()
		if on_confirmed.is_valid():
			on_confirmed.call(all_ok)
	dialog.confirmed.connect(cleanup)
	dialog.canceled.connect(dialog.queue_free)
	dialog.close_requested.connect(dialog.queue_free)
	base.add_child(dialog)
	dialog.popup_centered()
	dialog.get_ok_button().call_deferred("grab_focus")


## Simple confirmation; [param on_confirmed] on OK.
static func show_confirm_dialog(
	editor_interface: EditorInterface,
	title: String,
	message: String,
	on_confirmed: Callable = Callable()
) -> void:
	if not editor_interface:
		return
	var base = editor_interface.get_base_control()
	if not base:
		return
	var dialog = ConfirmationDialog.new()
	dialog.title = title
	dialog.dialog_text = message
	dialog.theme = editor_interface.get_editor_theme()
	dialog.min_size = Vector2i(420, 0)
	var cleanup = func():
		dialog.queue_free()
		if on_confirmed.is_valid():
			on_confirmed.call()
	dialog.confirmed.connect(cleanup)
	dialog.canceled.connect(dialog.queue_free)
	dialog.close_requested.connect(dialog.queue_free)
	base.add_child(dialog)
	dialog.popup_centered()
	dialog.get_ok_button().call_deferred("grab_focus")


## Backup prompt; [param on_confirmed] gets [code]cancel_requested[/code] (true = cancel/close).
static func show_backup_confirm_dialog(
	editor_interface: EditorInterface, on_confirmed: Callable
) -> void:
	if not editor_interface:
		on_confirmed.call(false)
		return
	var base = editor_interface.get_base_control()
	if not base:
		on_confirmed.call(false)
		return
	var dialog = ConfirmationDialog.new()
	dialog.title = UIStrings.DIALOG_BACKUP_TITLE
	dialog.dialog_text = UIStrings.DIALOG_BACKUP_MESSAGE
	dialog.theme = editor_interface.get_editor_theme()
	dialog.ok_button_text = UIStrings.BTN_CONTINUE
	dialog.cancel_button_text = UIStrings.BTN_CANCEL
	var cleanup = func(canceled: bool):
		dialog.queue_free()
		if on_confirmed.is_valid():
			on_confirmed.call(canceled)
	dialog.confirmed.connect(cleanup.bind(false))
	dialog.canceled.connect(cleanup.bind(true))
	dialog.close_requested.connect(cleanup.bind(true))
	base.add_child(dialog)
	dialog.popup_centered()
	dialog.get_ok_button().call_deferred("grab_focus")

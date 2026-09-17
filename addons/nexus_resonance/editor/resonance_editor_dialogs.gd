@tool
extends RefCounted

## Dialogs and toasts for the addon (consistent with editor theme).

const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")


## Icon from [param icon_path], else theme icon [param fallback_icon]. Null if [param base] is null.
static func get_icon(base: Control, icon_path: String, fallback_icon: String) -> Texture2D:
	if not base:
		return null
	var icon = load(icon_path) as Texture2D if ResourceLoader.exists(icon_path) else null
	if icon:
		return icon
	return base.get_theme_icon(fallback_icon, "EditorIcons")


## Modal error (blocking).
static func show_critical(
	editor_interface: EditorInterface, message: String, title: String = ""
) -> void:
	var t = title if not title.is_empty() else UIStrings.DIALOG_BAKE_FAILED_TITLE
	show_error_dialog(editor_interface, t, message)


## Non-blocking warning (EditorToaster when available).
static func show_warning(editor_interface: EditorInterface, message: String) -> void:
	if _try_push_toast(editor_interface, message, 1):
		return
	push_warning(UIStrings.PREFIX + message)


## Prints colored line (no dialog).
static func show_info(message: String) -> void:
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
	# Non-exclusive: progress UI or a prior dialog must not fight Godot's single exclusive child.
	dialog.exclusive = false
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

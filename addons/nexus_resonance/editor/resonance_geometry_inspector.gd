@tool
extends EditorInspectorPlugin

## Inspector toolbar: export ResonanceDynamicGeometry to a mesh asset.

const ResonancePaths = preload("res://addons/nexus_resonance/scripts/resonance_paths.gd")
const ResonanceFsPaths = preload("res://addons/nexus_resonance/scripts/resonance_fs_paths.gd")
const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")
const ResonanceEditorDialogs = preload(
	"res://addons/nexus_resonance/editor/resonance_editor_dialogs.gd"
)

var editor_interface: EditorInterface = null


func _can_handle(object: Object) -> bool:
	return object != null and object.is_class("ResonanceDynamicGeometry")


func _parse_begin(object: Object) -> void:
	var hbox = HBoxContainer.new()
	hbox.add_theme_constant_override("separation", 4)
	var btn = Button.new()
	btn.text = UIStrings.BTN_EXPORT_MESH
	btn.tooltip_text = UIStrings.TT_EXPORT_MESH
	var base = editor_interface.get_base_control() if editor_interface else null
	btn.icon = ResonanceEditorDialogs.get_icon(base, UIStrings.ICON_EXPORT, "Export")
	btn.pressed.connect(_on_export_pressed.bind(object))
	hbox.add_child(btn)
	add_custom_control(hbox)


func _on_export_pressed(obj: Object) -> void:
	if not obj or not obj.is_class("ResonanceDynamicGeometry"):
		return
	var geom: Node = obj
	var parent = geom.get_parent()
	var parent_name = parent.name if parent else "mesh"
	var dynamics_dir: String = ResonancePaths.get_dynamics_dir()
	var fs_dynamics: String = ResonanceFsPaths.filesystem_path_for_dir_access(dynamics_dir)
	if not DirAccess.dir_exists_absolute(fs_dynamics):
		var mk_err: int = DirAccess.make_dir_recursive_absolute(fs_dynamics)
		if mk_err != OK or not DirAccess.dir_exists_absolute(fs_dynamics):
			if editor_interface:
				ResonanceEditorDialogs.show_error_dialog(
					editor_interface,
					tr(UIStrings.DIALOG_EXPORT_FAILED_TITLE),
					tr(UIStrings.ERR_MKDIR_OUTPUT_DIR) % [dynamics_dir, mk_err],
					"",
					""
				)
			else:
				push_error(
					UIStrings.PREFIX + (tr(UIStrings.ERR_MKDIR_OUTPUT_DIR) % [dynamics_dir, mk_err])
				)
			return
	var save_path = ResonancePaths.dynamic_mesh_asset_save_path(str(parent_name).to_snake_case())
	var err: int = geom.export_dynamic_mesh_to_asset(save_path)
	if err == OK:
		if editor_interface:
			ResonanceEditorDialogs.show_success_toast(
				editor_interface, tr(UIStrings.INFO_DYNAMIC_EXPORTED) % save_path
			)
			if editor_interface.get_resource_filesystem():
				editor_interface.get_resource_filesystem().scan()
		else:
			ResonanceEditorDialogs.show_info(tr(UIStrings.INFO_DYNAMIC_EXPORTED) % save_path)
	else:
		if editor_interface:
			ResonanceEditorDialogs.show_error_dialog(
				editor_interface,
				tr(UIStrings.DIALOG_EXPORT_FAILED_TITLE),
				tr(UIStrings.ERR_EXPORT_FAILED) % err,
				"Export returned error %s." % err,
				"Ensure the mesh has valid geometry and %s is writable." % dynamics_dir
			)
		else:
			push_error(UIStrings.PREFIX + (tr(UIStrings.ERR_EXPORT_FAILED) % err))

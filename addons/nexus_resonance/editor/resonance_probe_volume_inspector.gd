@tool
extends EditorInspectorPlugin

## ResonanceProbeVolume inspector: bake controls. [member bake_runner] is set by the plugin.

const UIStrings = preload("res://addons/nexus_resonance/scripts/resonance_ui_strings.gd")
const ResonanceEditorDialogs = preload(
	"res://addons/nexus_resonance/editor/resonance_editor_dialogs.gd"
)
const ResonanceBakeDiscovery = preload(
	"res://addons/nexus_resonance/editor/resonance_bake_discovery.gd"
)
const ResonanceProbeVolumeDefaults = preload(
	"res://addons/nexus_resonance/scripts/resonance_probe_volume_defaults.gd"
)

var bake_runner = null
var editor_interface: EditorInterface = null


func _can_handle(object: Object) -> bool:
	return object != null and object.is_class("ResonanceProbeVolume")


func _parse_begin(object: Object) -> void:
	if not editor_interface:
		return

	# Node creation in the editor often skips _ready; ensure defaults when inspecting.
	if ResonanceProbeVolumeDefaults.ensure_resources(object):
		editor_interface.mark_scene_as_unsaved()
		if object is Object and object.has_method("notify_property_list_changed"):
			object.notify_property_list_changed()

	var base: Control = editor_interface.get_base_control() if editor_interface else null

	# Bake Probes
	var btn = Button.new()
	btn.text = tr(UIStrings.BTN_BAKE_PROBES)
	btn.tooltip_text = tr(UIStrings.TT_BAKE_PROBES)
	btn.icon = ResonanceEditorDialogs.get_icon(base, UIStrings.ICON_BAKE, "Bake")
	btn.pressed.connect(_on_bake_pressed.bind(object))
	add_custom_control(btn)


func _parse_property(
	object: Object,
	_type: Variant.Type,
	name: String,
	_hint_type: PropertyHint,
	_hint_string: String,
	_usage_flags: int,
	_wide: bool
) -> bool:
	# Insert below scan_targets: controls added here appear before bake_sources.
	if name != "bake_sources":
		return false
	var base: Control = editor_interface.get_base_control() if editor_interface else null
	var update_btn := Button.new()
	update_btn.text = tr(UIStrings.BTN_UPDATE_TARGETS)
	update_btn.tooltip_text = tr(UIStrings.TT_UPDATE_TARGETS)
	update_btn.icon = base.get_theme_icon("Reload", "EditorIcons") if base else null
	update_btn.pressed.connect(_on_update_targets_pressed.bind(object))
	add_custom_control(update_btn)
	add_custom_control(HSeparator.new())
	return false


func _on_update_targets_pressed(vol: Object) -> void:
	if vol == null or not vol.is_class("ResonanceProbeVolume"):
		return
	var result: Dictionary = ResonanceBakeDiscovery.update_volume_bake_targets_from_scan(vol as Node)
	if editor_interface:
		editor_interface.mark_scene_as_unsaved()
	print(
		(
			"Nexus Resonance: Updated bake targets (%s sources, %s listeners from %s scan roots)."
			% [result.get("sources", 0), result.get("listeners", 0), result.get("scan_roots_used", 0)]
		)
	)


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

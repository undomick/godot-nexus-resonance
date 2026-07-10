extends RefCounted

## Central UI string constants for Nexus Resonance addon.
## Use tr() at display sites for i18n. Add .po/.csv for target languages.

# --- Addon ---
const ADDON_NAME := "Nexus Resonance"
const PREFIX := "Nexus Resonance: "

# --- Buttons ---
const BTN_BAKE_PROBES := "Bake Probes"
const BTN_EXPORT_MESH := "Export Mesh"
const BTN_CLEAR := "Clear"
const BTN_CANCEL := "Cancel"
const BTN_CONTINUE := "Continue"
const BTN_BAKE_ANYWAY := "Bake Anyway"
const BTN_UNDO := "Undo"
const BTN_PREVIEW_BAKE_SETTINGS := "Preview Bake Settings"
const BTN_DOCUMENTATION := "Documentation"

# --- Menu (Project > Tools > Nexus Resonance) ---
const MENU_EXPORT_ACTIVE_SCENE := "Export Active Scene"
const MENU_EXPORT_ALL_OPEN_SCENES := "Export All Open Scenes"
const MENU_EXPORT_ACTIVE_SCENE_OBJ := "Export Active Scene To OBJ (Debug)"
const MENU_EXPORT_DYNAMIC_OBJECTS_ACTIVE := "Export Dynamic Objects In Active Scene"
const MENU_EXPORT_DYNAMIC_OBJECTS_IN_BUILD := "Export Dynamic Objects In All Scenes In Build"
const MENU_EXPORT_DYNAMIC_OBJECTS_IN_PROJECT := "Export All Dynamic Objects In Project"
const MENU_CLEAR_UNREFERENCED_PROBE_DATA := "Clear Unreferenced Probe Data"
const MENU_BAKE_ALL_PROBE_VOLUMES := "Bake All Probe Volumes In Active Scene"
const MENU_CLEAR_PROBE_BATCHES := "Clear Probe Batches"
const MENU_UNLINK_PROBE_VOLUME_REFS := "Unlink Probe Volume References"
const MENU_CONVERT_ANIMATION_AUDIO_RESONANCE := "Convert Animation Audio Tracks (ResonancePlayer)…"
const MENU_CONVERT_ANIMATION_AUDIO_ALL_SCENES := "Convert Animation Audio Tracks In All Scenes…"

# --- Dialogs ---
const DIALOG_BAKE_FAILED_TITLE := "Nexus Resonance - Bake Failed"
const DIALOG_EXPORT_FAILED_TITLE := "Nexus Resonance - Export Failed"
const DIALOG_SAVE_FAILED_TITLE := "Nexus Resonance - Save Failed"
const DIALOG_GDEXTENSION_NOT_LOADED_TITLE := "Nexus Resonance - GDExtension Not Loaded"
const DIALOG_BAKE_VALIDATION_TITLE := "Nexus Resonance - Pre-Bake Check"
const DIALOG_PROGRESS_TITLE := "Nexus Resonance - Baking"
const DIALOG_BACKUP_TITLE := "Nexus Resonance - Backup"
const DIALOG_BACKUP_MESSAGE := "A backup of current Probe Volume data will be created before baking. You can undo the bake to restore the previous data."

# --- Progress ---
const PROGRESS_PREPARING := "Preparing..."
const PROGRESS_PROCESSING := "Processing..."
const PROGRESS_BAKING_REVERB := "Baking reverb probes"
const PROGRESS_BAKING_PATHING := "Baking pathing"
const PROGRESS_BAKING_STATIC_SOURCE := "Baking static source"
const PROGRESS_BAKING_STATIC_LISTENER := "Baking static listener"
const PROGRESS_SKIPPING := "Skipping (up-to-date)"
const PROGRESS_ESTIMATED_TIME := "Estimated time: %s"
const PROGRESS_STAGE := "Stage %d of %d"
const PROGRESS_PROBES := "%d probes"
const PROGRESS_DETAILS := "Details"
const PROGRESS_STATUS := "Status"

# --- Errors (critical = dialog) ---
const ERR_NO_SCENE := "No scene open."
const ERR_EDITOR_FILESYSTEM_UNAVAILABLE := "Editor file system not available."
const ERR_EDITOR_FILESYSTEM_ROOT := "Could not scan res://."
const ERR_GDEXTENSION_NOT_LOADED := "GDExtension not loaded."
const ERR_EXPORT_FAILED := "Export failed with error %s"
const ERR_SCENE_NOT_EXPORTED := "Scene not exported. Use Tools > Nexus Resonance > Export Active Scene before baking."
const ERR_BAKE_RUNNER_NOT_INIT := "Bake runner not initialized."
const ERR_SERVER_LACKS_EXPORT := "ResonanceServer lacks export_static_scene_to_asset. Update the addon."
const ERR_SOFAAsset_UNAVAILABLE := "ResonanceSOFAAsset not available."
const ERR_BAKE_FAILED := "Bake failed. Check Editor output."
const ERR_CONFIG_FAILED := "Failed to build config."
const ERR_FAILED_TO_SAVE := "Failed to save: %s"
const ERR_NO_MAIN_SCENE := "No main scene configured."
const ERR_FAILED_TO_LOAD_MAIN_SCENE := "Failed to load main scene: %s"
const ERR_GDEXTENSION_SYNC := "Addon and GDExtension may be out of sync."
const ERR_GDEXTENSION_SYNC_SOLUTION := "Update the Nexus Resonance addon and GDExtension to matching versions."
const ERR_SET_MAIN_SCENE := "Set application/run/main_scene in Project Settings."
const ERR_MKDIR_AUDIO_DATA := "Failed to create audio_data directory (error %d). Ensure res:// is writable."
const ERR_MKDIR_RESONANCE_MESHES := "Failed to create resonance_meshes directory (error %d). Ensure res:// is writable."

# --- Warnings (notification) ---
const WARN_NO_SCENE := "No scene open."
const WARN_NO_DYNAMIC_GEOMETRY := "No ResonanceDynamicGeometry in scene. Nothing to export."
const WARN_SELECT_PROBE_VOLUMES := "Select one or more ResonanceProbeVolume nodes, then run this to unlink references before deleting."
const WARN_NO_PLAYER_REFS := "No ResonancePlayer in this scene points to the selected Probe Volume(s)."
const WARN_NO_PROBE_VOLUMES := "No ResonanceProbeVolume in scene. Nothing to bake."
const WARN_ADD_RUNTIME := "Add ResonanceRuntime with valid ResonanceRuntimeConfig to the scene."
## Shown when the editor needs ResonanceServer but the edited scene has no usable ResonanceRuntime (inspector / bake prep).
const WARN_RUNTIME_REQUIRED_EDITOR := (
	"No ResonanceRuntime with a valid ResonanceRuntimeConfig was found in the edited scene. "
	+ "Add a ResonanceRuntime node, assign a config resource, then deselect and reselect the ResonanceProbeVolume or use Bake Probes so the server can start."
)
const WARN_SERVER_INIT_FAILED := "Server init failed."
const WARN_BAKE_RUNNER_NOT_SET := "Bake runner not set. Cannot bake."
const WARN_NO_RESONANCE_RUNTIME := "Scene has no ResonanceRuntime. Add ResonanceRuntime for export to make sense."
const WARN_NO_EXPORTABLE_STATIC_CONTENT := "Scene has no ResonanceStaticGeometry or ResonanceStaticScene. Add ResonanceStaticGeometry for export."
const WARN_STATIC_SCENE_NO_ASSET_STILL_MERGED := (
	"ResonanceStaticScene at %s has no static_scene_asset; nested ResonanceStaticGeometry is still merged into this export. "
	+ "Export the sub-scene first to assign an asset, or remove the node - then parent exports will skip this subtree."
)
const WARN_NO_SCENES_OPEN := "No scenes open."
const WARN_NO_SCENES_EXPORTED := "No scenes exported."
const WARN_NO_SCENE_FILES := "No scene files found."
const WARN_NO_DYNAMIC_EXPORTED := "No dynamic objects exported."
const WARN_EXPORTED_BUT_SAVE_FAILED := "Exported %d mesh(es) but failed to save scene: %s"
const WARN_SAVE_SCENE_TO_PERSIST := " - Save scene to persist mesh_asset."

# --- Info (console) ---
const DIALOG_EXPORT_JOB_TITLE := "Nexus Resonance - Export"
const INFO_EXPORT_JOB_RUNNING := "Export in progress..."
const WARN_EXPORT_JOB_ALREADY_RUNNING := "An export is already running. Wait for it to finish or cancel the progress dialog."
const INFO_STATIC_UNCHANGED := "Static geometry unchanged (hash match). Skipping export."
const INFO_STATIC_EXPORTED := "Static scene exported to %s. ResonanceStaticScene updated."
const INFO_DYNAMIC_EXPORTED := "Dynamic mesh exported to %s"
const INFO_DYNAMIC_MESHES_EXPORTED := "Exported %d dynamic mesh(es)."
const INFO_DYNAMIC_OBJECTS_IN_BUILD_EXPORTED := "Exported %d dynamic object(s) from scenes in build."
const INFO_DYNAMIC_OBJECTS_IN_PROJECT_EXPORTED := "Exported %d dynamic object(s) from project."
const INFO_UNLINK_DONE := "Cleared %d pathing_probe_volume reference(s). You can now delete the Probe Volume(s)."
const INFO_PROBE_BATCHES_CLEARED := "Probe batches cleared."
const INFO_SCENE_OBJ_EXPORTED := "Scene exported to OBJ: %s"
const INFO_ALL_OPEN_SCENES_EXPORTED := "Exported %d open scene(s) to static assets."
const INFO_UNREFERENCED_PROBE_DATA_CLEARED := "Cleared %d unreferenced probe data file(s)."
const INFO_SCENES_FILTERED := "Skipped %d scene(s) without exportable static geometry."
const INFO_BACKUP_RESTORED := "Restored Probe Volume data from backup."
const INFO_NO_PROBE_DATA_FILES := "No probe data files found in %s"
const INFO_ALL_PROBE_DATA_REFERENCED := "All probe data files are still referenced. Nothing to clear."
const INFO_DELETE_UNREFERENCED_PROBE_DATA := "Delete %d unreferenced probe data file(s)?\n\n%s"
const DIALOG_CLEAR_UNREFERENCED_TITLE := "Clear Unreferenced Probe Data"
const DIALOG_CONVERT_ALL_SCENES_TITLE := "Nexus Resonance - Convert All Scenes"
const DIALOG_CONVERT_ALL_SCENES_MESSAGE := (
	"This will load every .tscn under res:// from disk, convert Animation audio tracks that target ResonancePlayer into Call Method tracks, and overwrite files that change.\n\n"
	+ "Use version control or a backup. Save or close open scenes first so you do not lose unsaved editor changes.\n\n"
	+ "Tracks with use blend are skipped (see Output for warnings). Continue?"
)
const INFO_AUTO_CONVERT_ANIMATION_PRE_SAVE := (
	"Auto-converted %d animation audio track(s) on ResonancePlayer (now Call Method). "
	+ "If any tracks used blend, check Output for Nexus Resonance warnings."
)
const INFO_CONVERT_ALL_SCENES_SUMMARY := (
	"Project-wide conversion: %d track(s) converted, %d scene file(s) saved, %d scene(s) failed. "
	+ "Skipped %d blend track(s), %d non-ResonancePlayer target(s)."
)
const INFO_CONVERT_SKIPPED_BLEND := " Skipped %d audio track(s) with use blend - convert manually (method keys) for full Steam Audio."

# --- Tooltips ---
const TT_BAKE_PROBES := "Bake reflections, pathing, static source/listener. Skips up-to-date stages. Configure in bake_config."
const TT_EXPORT_MESH := "Export this dynamic mesh to a .tres asset."
const TT_PREVIEW_BAKE := "Estimate probe count and approximate bake time for current settings."
const TT_CANCEL_BAKE := "Stop the current bake and close."
const TT_HELP := "Open documentation"

# --- Inspector (Probe Volume) ---
const INFO_CONFIGURE_BAKE_CONFIG_FOR_ESTIMATES := "Configure bake_config for accurate estimates."
const INSPECTOR_PROBE_BATCH_EDIT := "Probe batch (advanced)"
const BTN_REMOVE_PROBE_AT_INDEX := "Remove probe at index"
const BTN_REMOVE_BAKED_PATHING := "Remove baked pathing layer"
const BTN_REMOVE_BAKED_REFLECTION_REVERB := "Remove baked reflections (reverb)"
const TT_REMOVE_PROBE := "Uses Steam Audio iplProbeBatchRemoveProbe. Clears pathing hash; re-bake pathing if needed. Saves external .tres when set; reloads batch on this volume."
const TT_REMOVE_PATHING := "iplProbeBatchRemoveData: pathing (dynamic) layer. Reload probe batch after."
const TT_REMOVE_REFLECTION_REVERB := "iplProbeBatchRemoveData: reflections reverb layer. Re-bake probes if you still need this data."
const WARN_PROBE_EDIT_NO_SERVER := "ResonanceServer not ready. Add ResonanceRuntime with config to this scene, then click Bake Probes or re-open the inspector."
const WARN_PROBE_EDIT_FAILED := "Probe batch edit failed (wrong index, no matching baked layer, load/save error). See the Output panel for the exact message."
const INSPECTOR_PROBE_BATCH_SERVER_REQUIRED := "ResonanceServer is not initialized. Add a ResonanceRuntime node (with ResonanceRuntimeConfig) to the edited scene, then press Bake Probes once or deselect and reselect this volume so the server can start. Without it, probe batch edits cannot run."
const INSPECTOR_PROBE_COUNT_KNOWN := "Probes (from data): %d"
const INSPECTOR_PROBE_COUNT_UNKNOWN := "Probes (from data): (initialize server to query)"

# --- Gizmo (Probe Volume) ---
const GIZMO_PROBE_VOLUME_CLASS := "ResonanceProbeVolume"
const ICON_PROBE_VOLUME_GIZMO := "res://addons/nexus_resonance/ui/icons/probe_volume_gizmo.svg"
const ICON_FMOD_EVENT_EMITTER := "res://addons/nexus_resonance/ui/icons/fmod_emitter_icon.svg"
const ICON_CODA_EVENT_EMITTER := "res://addons/nexus_resonance/ui/icons/coda_emitter_icon.svg"
const GIZMO_HANDLE_SIZE_PX := "Size +X"
const GIZMO_HANDLE_SIZE_PY := "Size +Y"
const GIZMO_HANDLE_SIZE_PZ := "Size +Z"
const GIZMO_HANDLE_SIZE_MX := "Size -X"
const GIZMO_HANDLE_SIZE_MY := "Size -Y"
const GIZMO_HANDLE_SIZE_MZ := "Size -Z"
const UNDO_CHANGE_PROBE_VOLUME_SIZE := "Change Resonance Probe Volume Size"

# --- Icon paths (res://addons/nexus_resonance/ui/icons/) ---
const ICON_BAKE := "res://addons/nexus_resonance/ui/icons/icon_bake.svg"
const ICON_EXPORT := "res://addons/nexus_resonance/ui/icons/icon_export.svg"
const ICON_CLEAR := "res://addons/nexus_resonance/ui/icons/icon_clear.svg"
const ICON_HELP := "res://addons/nexus_resonance/ui/icons/icon_help.svg"

# --- Docs ---
## Base URL for documentation. Anchors (#bake-workflow, #probe-volume, #export) must match
## the target docs structure. Update when docs are relocated.
const DOC_BASE_URL := "https://github.com/nexus-resonance/docs"
const DOC_BAKE_WORKFLOW := DOC_BASE_URL + "#bake-workflow"
const DOC_PROBE_VOLUME := DOC_BASE_URL + "#probe-volume"
const DOC_EXPORT := DOC_BASE_URL + "#export"

static var _translation_en: Translation = null
static var _translation_es: Translation = null

static func localize(key: String) -> String:
	if _translation_en == null:
		_translation_en = load("res://addons/nexus_resonance/translations/nexus_resonance.en.translation") as Translation
	if _translation_es == null:
		_translation_es = load("res://addons/nexus_resonance/translations/nexus_resonance.es.translation") as Translation
	
	var locale: String = "en"
	if Engine.is_editor_hint():
		locale = TranslationServer.get_tool_locale()
	else:
		locale = TranslationServer.get_locale()
	
	var is_es = locale.begins_with("es")
	var trans_res = _translation_es if is_es else _translation_en
	if trans_res:
		var msg = trans_res.get_message(key)
		if not msg.is_empty():
			return msg
	return key


@tool
extends EditorInspectorPlugin

## Adds a DSP limitation note to ResonanceFmodEventEmitter inspector.
## Users see the note without reading developer docs.

const DSP_LIMITATION_NOTE := "Steam Audio DSP parameter binding is pending fmod-gdextension API support. Position sync works; full spatialization may be limited until then."


func _can_handle(object: Object) -> bool:
	return object != null and object.get_class() == "ResonanceFmodEventEmitter"


func _parse_begin(_object: Object) -> void:
	var lbl := Label.new()
	lbl.text = DSP_LIMITATION_NOTE
	lbl.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	lbl.add_theme_color_override("font_color", Color(0.7, 0.7, 0.5))
	lbl.add_theme_font_size_override("font_size", 11)
	add_custom_control(lbl)

extends CanvasLayer

## Minimal performance overlay: FPS, frame time, and physics step time (Godot monitors only).
## Toggle with the key set on [member ResonanceRuntime.performance_overlay_toggle_key] (default F2) when [member ResonanceRuntime.enable_debug] is on.

var _panel: PanelContainer
var _label: RichTextLabel
var _update_timer: float = 0.0
const UPDATE_INTERVAL: float = 0.2
const COLOR_NEUTRAL := "#dddddd"


func _ready() -> void:
	if Engine.is_editor_hint():
		return
	_build_ui()
	get_viewport().size_changed.connect(_update_position)


func _build_ui() -> void:
	layer = 101  # Above debug overlay (100)
	_panel = PanelContainer.new()
	var style = StyleBoxFlat.new()
	style.bg_color = Color(0, 0, 0, 0.2)
	style.border_color = Color(0.4, 0.6, 0.9)
	style.set_border_width_all(0)
	style.set_corner_radius_all(2)
	style.set_content_margin_all(8)
	_panel.add_theme_stylebox_override("panel", style)

	_label = RichTextLabel.new()
	_label.bbcode_enabled = true
	_label.fit_content = true
	_label.scroll_active = false
	_label.autowrap_mode = TextServer.AUTOWRAP_OFF
	_label.add_theme_font_size_override("normal_font_size", 14)
	_label.text = "Performance"
	_label.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	_label.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	_panel.add_child(_label)
	add_child(_panel)

	# Top-left anchors only: no vertical stretch to viewport (common default under CanvasLayer).
	_panel.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_panel.custom_minimum_size = Vector2(160, 0)
	_panel.size_flags_horizontal = Control.SIZE_SHRINK_BEGIN
	_panel.size_flags_vertical = Control.SIZE_SHRINK_BEGIN
	visible = false
	call_deferred("_update_position")


func _exit_tree() -> void:
	visible = false
	process_mode = Node.PROCESS_MODE_DISABLED


func _update_position() -> void:
	if not _panel:
		return
	var vp = get_viewport()
	if vp:
		var vs: Vector2 = vp.get_visible_rect().size
		var w: float = maxf(_panel.get_combined_minimum_size().x, _panel.custom_minimum_size.x)
		_panel.position = Vector2(vs.x - w - 20, 10)


func _process(delta: float) -> void:
	if Engine.is_editor_hint():
		return
	if not visible:
		return
	if not _label:
		return

	_update_timer += delta
	if _update_timer >= UPDATE_INTERVAL:
		_update_timer = 0.0
		_refresh()


func _refresh() -> void:
	var fps := Performance.get_monitor(Performance.TIME_FPS)
	var process_ms := Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0
	var physics_ms := Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0
	_label.text = (
		("[color={c}]FPS: {fps}[/color]\n[color={c}]Frame: {proc_ms} ms[/color]\n[color={c}]Physics: {phy_ms} ms[/color]")
		. format(
			{
				"c": COLOR_NEUTRAL,
				"fps": int(fps),
				"proc_ms": String.num(process_ms, 2),
				"phy_ms": String.num(physics_ms, 2),
			}
		)
	)
	_panel.reset_size()
	call_deferred("_update_position")

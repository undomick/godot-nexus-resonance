extends Node

## Category-filtered logger (Autoload [code]ResonanceLogger[/code], not [code]class_name[/code]).
## [method log] from GDScript; C++ via [code]Engine.get_singleton[/code]. File queue + mutex; flush in [method _process].

const DEFAULT_BUFFER_SIZE := 128
const PROJECT_PREFIX := "nexus/nexus_resonance/logger/"

## Filter keys
const CATEGORY_REFLECTIONS := &"reflections"
const CATEGORY_REALTIME_RAYS := &"realtime_rays"
const CATEGORY_SOURCE_VOLUME := &"source_volume"
const CATEGORY_PATHING := &"pathing"
const CATEGORY_OCCLUSION := &"occlusion"
const CATEGORY_INIT := &"init"
const CATEGORY_BAKE := &"bake"
const CATEGORY_VALIDATION := &"validation"

const ALL_CATEGORIES: Array[StringName] = [
	CATEGORY_REFLECTIONS,
	CATEGORY_REALTIME_RAYS,
	CATEGORY_SOURCE_VOLUME,
	CATEGORY_PATHING,
	CATEGORY_OCCLUSION,
	CATEGORY_INIT,
	CATEGORY_BAKE,
	CATEGORY_VALIDATION
]


## Default categories dict (all on).
static func get_default_categories_enabled_dict() -> Dictionary:
	var d := {}
	for c in ALL_CATEGORIES:
		d[str(c)] = true
	return d


## Fired after filter: category, message, data
signal log_entry_added(category: StringName, message: String, data: Dictionary)

var _buffer: Array[Dictionary] = []
var _buffer_size: int = DEFAULT_BUFFER_SIZE
var _categories_enabled: Dictionary = {}
var _output_to_debug: bool = true
var _output_to_file: bool = false
var _file_path: String = "user://nexus_resonance_log.ndjson"
var _file_write_queue: Array = []
var _file_write_mutex: Mutex = Mutex.new()

var _tts_runtime_enabled: bool = false
var _last_player_states: Dictionary = {}
var _current_probe_volume_path: NodePath = NodePath()


## [code]user://[/code] / [code]res://[/code] only; no [code]..[/code].
static func _is_safe_log_path(path: String) -> bool:
	if path.is_empty():
		return false
	if not path.begins_with("user://") and not path.begins_with("res://"):
		return false
	if ".." in path or "/../" in path or path.ends_with("/.."):
		return false
	return true


func _init() -> void:
	_load_category_defaults()
	for cat in ALL_CATEGORIES:
		if not _categories_enabled.has(cat):
			_categories_enabled[cat] = true


func _ready() -> void:
	# Settings available after entering the tree.
	_load_category_defaults()
	
	if ProjectSettings.has_setting("nexus/nexus_resonance/accessibility/runtime_debug_tts"):
		_tts_runtime_enabled = ProjectSettings.get_setting("nexus/nexus_resonance/accessibility/runtime_debug_tts", false)
	
	if _tts_runtime_enabled and not Engine.is_editor_hint():
		var timer := Timer.new()
		timer.wait_time = 0.3
		timer.autostart = true
		timer.timeout.connect(_on_monitor_timeout)
		add_child(timer)
		_speak("Depurador de accesibilidad acústica activado.")


func _on_monitor_timeout() -> void:
	_monitor_players()
	_monitor_listener_reverb_zone()


func _monitor_players() -> void:
	var players = get_tree().get_nodes_in_group("resonance_player")
	var current_states := {}
	for p in players:
		if is_instance_valid(p) and p.has_method("is_playing"):
			var path := p.get_path()
			var playing: bool = p.is_playing()
			current_states[path] = playing
			if not _last_player_states.has(path):
				_last_player_states[path] = false
			if _last_player_states[path] != playing:
				if playing:
					_speak("Reproduciendo fuente " + p.name)
				else:
					_speak("Fuente " + p.name + " detenida")
	_last_player_states = current_states


func _monitor_listener_reverb_zone() -> void:
	var listeners = get_tree().get_nodes_in_group("resonance_listener")
	if listeners.is_empty():
		return
	var listener = listeners[0]
	if not is_instance_valid(listener):
		return
	
	var l_pos: Vector3 = listener.global_position
	var volumes = get_tree().get_nodes_in_group("resonance_probe_volume")
	var inside_volume: Node = null
	
	for vol in volumes:
		if is_instance_valid(vol) and vol.has_method("get_region_size"):
			var size: Vector3 = vol.get_region_size()
			var v_pos: Vector3 = vol.global_position
			var half_size := size / 2.0
			var min_bound := v_pos - half_size
			var max_bound := v_pos + half_size
			
			if (l_pos.x >= min_bound.x and l_pos.x <= max_bound.x and
				l_pos.y >= min_bound.y and l_pos.y <= max_bound.y and
				l_pos.z >= min_bound.z and l_pos.z <= max_bound.z):
					inside_volume = vol
					break
	
	var new_path = inside_volume.get_path() if inside_volume else NodePath()
	if _current_probe_volume_path != new_path:
		if inside_volume:
			_speak("Entrando en zona acústica " + inside_volume.name)
		else:
			_speak("Saliendo de zona acústica")
		_current_probe_volume_path = new_path


func _process(_delta: float) -> void:
	if not _output_to_file:
		return
	_file_write_mutex.lock()
	var to_write: Array = _file_write_queue.duplicate()
	_file_write_queue.clear()
	_file_write_mutex.unlock()
	for entry in to_write:
		_write_to_file(entry)


func _load_category_defaults() -> void:
	if ProjectSettings.has_setting(PROJECT_PREFIX + "categories_enabled"):
		var v = ProjectSettings.get_setting(PROJECT_PREFIX + "categories_enabled")
		if v is Dictionary:
			if v.is_empty():
				for cat in ALL_CATEGORIES:
					_categories_enabled[cat] = true
			else:
				for k in v:
					_categories_enabled[StringName(str(k))] = bool(v[k])
	if ProjectSettings.has_setting(PROJECT_PREFIX + "output_to_debug"):
		_output_to_debug = ProjectSettings.get_setting(PROJECT_PREFIX + "output_to_debug")
	if ProjectSettings.has_setting(PROJECT_PREFIX + "output_to_file"):
		_output_to_file = ProjectSettings.get_setting(PROJECT_PREFIX + "output_to_file")
	if ProjectSettings.has_setting(PROJECT_PREFIX + "file_path"):
		var configured: String = ProjectSettings.get_setting(PROJECT_PREFIX + "file_path")
		if _is_safe_log_path(configured):
			_file_path = configured
		else:
			push_warning(
				"Nexus Resonance: Logger file_path must be user:// or res:// (no path traversal). Using default."
			)


## Log if category enabled. [param data] optional.
func log(category: StringName, message: String, data: Dictionary = {}) -> void:
	if not _is_category_enabled(category):
		return

	var entry := {
		"timestamp": Time.get_ticks_msec(),
		"category": String(category),
		"message": message,
		"data": data.duplicate()
	}

	_add_to_buffer(entry)

	if _output_to_debug:
		_output_to_debug_console(category, message, data)

	if _output_to_file:
		_file_write_mutex.lock()
		_file_write_queue.append(entry)
		_file_write_mutex.unlock()

	if _tts_runtime_enabled and not Engine.is_editor_hint():
		if category == &"error" or category == &"validation" or category == &"init":
			_speak(message)

	log_entry_added.emit(category, message, data)


func _is_category_enabled(category: StringName) -> bool:
	if _categories_enabled.has(category):
		return _categories_enabled[category]
	return true


func _add_to_buffer(entry: Dictionary) -> void:
	_buffer.append(entry)
	while _buffer.size() > _buffer_size:
		_buffer.pop_front()


func _output_to_debug_console(category: StringName, message: String, data: Dictionary) -> void:
	var data_suffix := "" if data.is_empty() else " " + str(data)
	# Rich line + plain duplicate (some hosts only show print).
	print_rich(
		"[color=cyan][Nexus Resonance][%s][/color] %s%s" % [String(category), message, data_suffix]
	)
	print("[Nexus Resonance][%s] %s%s" % [String(category), message, data_suffix])


func _write_to_file(entry: Dictionary) -> void:
	if not _is_safe_log_path(_file_path):
		return
	var path := _file_path
	if path.begins_with("user://") or path.begins_with("res://"):
		path = ProjectSettings.globalize_path(path)
	var dict := {
		"timestamp": entry.timestamp,
		"category": entry.category,
		"message": entry.message,
		"data": entry.data
	}
	var line := JSON.stringify(dict) + "\n"
	var f: FileAccess = null
	# Append mode; avoid multiple writers to same file.
	if FileAccess.file_exists(path):
		f = FileAccess.open(path, FileAccess.READ_WRITE)
	if f == null:
		f = FileAccess.open(path, FileAccess.WRITE)
	if f == null:
		push_warning(
			(
				"Nexus Resonance: Cannot open log file for writing: %s (error %d)"
				% [path, FileAccess.get_open_error()]
			)
		)
		return
	f.seek_end()
	f.store_string(line)
	f.close()


## Toggle category (future [method log] calls).
func set_category_enabled(category: StringName, enabled: bool) -> void:
	_categories_enabled[category] = enabled


## Query category toggle
func is_category_enabled(category: StringName) -> bool:
	return _is_category_enabled(category)


## Ring buffer tail for UI
func get_recent_entries(count: int = 32) -> Array[Dictionary]:
	var start := maxi(0, _buffer.size() - count)
	var result: Array[Dictionary] = []
	for i in range(start, _buffer.size()):
		result.append(_buffer[i])
	return result


## Empty buffer
func clear_buffer() -> void:
	_buffer.clear()


func set_buffer_size(size: int) -> void:
	_buffer_size = maxi(16, size)
	while _buffer.size() > _buffer_size:
		_buffer.pop_front()


func set_output_to_debug(enabled: bool) -> void:
	_output_to_debug = enabled


func set_output_to_file(enabled: bool) -> void:
	_output_to_file = enabled


func set_file_path(path: String) -> void:
	if _is_safe_log_path(path):
		_file_path = path
	else:
		push_warning(
			"Nexus Resonance: Logger file_path must be user:// or res:// (no path traversal). Ignored."
		)


## All [StringName] categories (settings UI)
func get_all_categories() -> Array[StringName]:
	return ALL_CATEGORIES.duplicate()


func _speak(message: String) -> void:
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

extends RefCounted

## Reverb bus wiring for [ResonanceRuntime]: create/configure the wet bus and sync [ResonancePlayer] routing.

const EFFECT_CLASS := "ResonanceAudioEffect"


## Adds [ResonanceAudioEffect] at index 0 of the bus unless the bus already has one.
## Call when the reverb bus already exists from [AudioBusLayout] but the effect was stripped or failed to load.
static func ensure_resonance_reverb_effect_on_bus(bus_idx: int) -> void:
	if bus_idx < 0:
		return
	if not ClassDB.class_exists(EFFECT_CLASS):
		return
	var n := AudioServer.get_bus_effect_count(bus_idx)
	for i in range(n):
		var eff: AudioEffect = AudioServer.get_bus_effect(bus_idx, i)
		if eff != null and eff.get_class() == EFFECT_CLASS:
			return
	var effect: AudioEffect = ClassDB.instantiate(EFFECT_CLASS)
	effect.resource_name = "Resonance Reverb"
	AudioServer.add_bus_effect(bus_idx, effect, 0)


var _get_bus_effective: Callable
var _get_reverb_bus_name: Callable
## Returns the send target of the reverb bus (the dry [code]bus[/code]).
var _get_reverb_bus_send: Callable


## Separate from construction so native (C++) owners can instantiate without arguments.
func setup(
	get_bus_effective_cb: Callable,
	get_reverb_bus_name_cb: Callable,
	get_reverb_bus_send_cb: Callable
) -> void:
	_get_bus_effective = get_bus_effective_cb
	_get_reverb_bus_name = get_reverb_bus_name_cb
	_get_reverb_bus_send = get_reverb_bus_send_cb


func get_bus_effective() -> StringName:
	return _get_bus_effective.call()


func get_reverb_bus_name() -> StringName:
	return _get_reverb_bus_name.call()


func get_reverb_bus_send() -> StringName:
	return _get_reverb_bus_send.call()


## Creates the reverb bus if missing, sets its send and adds [ResonanceAudioEffect] to it.
func ensure_reverb_bus_exists() -> bool:
	var bus_name := get_reverb_bus_name()
	var send_name := get_reverb_bus_send()
	var idx = AudioServer.get_bus_index(bus_name)
	if idx == -1:
		AudioServer.add_bus()
		idx = AudioServer.bus_count - 1
		AudioServer.set_bus_name(idx, bus_name)
		if idx > 1:
			AudioServer.move_bus(idx, 1)
			idx = 1
	if idx >= 0:
		AudioServer.set_bus_send(idx, send_name)
		ensure_resonance_reverb_effect_on_bus(idx)
	return idx >= 0


## Pushes effective buses and reverb send buses to [code]resonance_player[/code] nodes.
## Pass [param players] when the caller already collected the group (e.g. [ResonanceRuntime] per-frame cache).
func apply_bus_to_players(tree: SceneTree, players: Array = []) -> void:
	if tree == null:
		return
	var global_bus := get_bus_effective()
	var global_reverb_bus := get_reverb_bus_name()
	var player_nodes: Array = players
	if player_nodes.is_empty():
		player_nodes = tree.get_nodes_in_group("resonance_player")
	for p in player_nodes:
		var cfg = p.get("player_config") if "player_config" in p else null
		var effective_bus: StringName
		if cfg and cfg.has_method("get_bus_name_effective"):
			effective_bus = cfg.get_bus_name_effective(global_bus)
		else:
			effective_bus = global_bus
		if p.has_method("set_bus"):
			p.set_bus(effective_bus)
		var effective_reverb_bus: StringName
		if cfg and cfg.has_method("get_reverb_bus_name_effective"):
			effective_reverb_bus = cfg.get_reverb_bus_name_effective(global_reverb_bus)
		else:
			effective_reverb_bus = global_reverb_bus
		var send_bus: StringName = (
			effective_reverb_bus if effective_bus != effective_reverb_bus else &""
		)
		if p.has_method("set_reverb_send_bus"):
			p.set_reverb_send_bus(send_bus)

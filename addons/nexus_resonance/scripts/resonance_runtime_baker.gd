extends RefCounted
class_name ResonanceRuntimeBaker

## Runtime bake API (exported builds, proc-gen, no editor). Wraps [ResonanceBakeRunner] headless.

const ResonanceSceneUtils = preload("res://addons/nexus_resonance/scripts/resonance_scene_utils.gd")

signal bake_progress_updated(status_message: String)
signal bake_finished

var _runner: ResonanceBakeRunner


func _init() -> void:
	_runner = ResonanceBakeRunner.new(null)

	_runner.bake_progress_updated.connect(func(msg: String): bake_progress_updated.emit(msg))
	_runner.bake_finished.connect(func(): bake_finished.emit())


## Bakes into memory only; nothing is saved to disk.
func bake_volumes_to_ram(volumes: Array[Node], scene_root: Node) -> void:
	if volumes.is_empty() or not scene_root:
		push_error("Nexus Resonance: Runtime bake needs volumes and scene_root.")
		bake_finished.emit.call_deferred()
		return

	_runner.run_bake(volumes, scene_root, false)


## Releases the loaded probe batch of each volume. probe_data stays assigned.
func flush_volumes(volumes: Array[Node]) -> void:
	for vol in volumes:
		if vol and vol.has_method("release_probe_batch"):
			vol.release_probe_batch()


## Releases the probe batches of all volumes under [param scene_root], e.g. on teardown or before a re-bake.
func flush_all_runtime_bakes(scene_root: Node) -> void:
	if not scene_root:
		return
	var collected: Array[Node] = []
	ResonanceSceneUtils.collect_resonance_probe_volumes(scene_root, collected)
	flush_volumes(collected)


## Reloads the probe batch of each volume, e.g. after a flush or an in-memory bake.
func reload_volumes(volumes: Array[Node]) -> void:
	for vol in volumes:
		if vol and vol.has_method("reload_probe_batch"):
			vol.reload_probe_batch()


## Reload all probe volumes under [param scene_root].
func reload_all_runtime_bakes(scene_root: Node) -> void:
	if not scene_root:
		return
	var collected: Array[Node] = []
	ResonanceSceneUtils.collect_resonance_probe_volumes(scene_root, collected)
	reload_volumes(collected)


## Shuts down the bake runner. The baker cannot be used afterwards.
func shutdown() -> void:
	if _runner and _runner.has_method("shutdown"):
		_runner.shutdown()
	_runner = null

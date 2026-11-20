extends GutTest

## The ResonanceLogger autoload is visible as an Engine singleton while it is in the tree, so
## C++ forwarding and Engine.get_singleton callers reach it.

const ResonanceLoggerScript = preload("res://addons/nexus_resonance/scripts/resonance_logger.gd")
const Constants = preload("res://addons/nexus_resonance/scripts/resonance_config_constants.gd")
const SETTLE_FRAMES := 4

var _logger: Node


func before_each() -> void:
	if Engine.has_singleton(ResonanceLoggerScript.SINGLETON_NAME):
		_logger = null
		return
	_logger = ResonanceLoggerScript.new()
	_logger.set_output_to_debug(false)
	add_child_autoqfree(_logger)


func test_logger_registers_while_in_tree() -> void:
	if _logger == null:
		pending("ResonanceLogger autoload is already registered in this project")
		return
	assert_same(Engine.get_singleton(ResonanceLoggerScript.SINGLETON_NAME), _logger)

	remove_child(_logger)
	assert_false(Engine.has_singleton(ResonanceLoggerScript.SINGLETON_NAME))

	add_child(_logger)
	assert_same(Engine.get_singleton(ResonanceLoggerScript.SINGLETON_NAME), _logger)


func test_native_validation_warning_reaches_logger() -> void:
	if _logger == null:
		pending("ResonanceLogger autoload is already registered in this project")
		return
	var config := ResonanceRuntimeConfig.new()
	config.reflection_type = Constants.REFLECTION_TYPE_PARAMETRIC
	var runtime: Node = ClassDB.instantiate("ResonanceRuntime")
	runtime.set("runtime", config)
	add_child_autoqfree(runtime)
	await wait_process_frames(SETTLE_FRAMES)

	var data: Resource = ClassDB.instantiate("ResonanceProbeData")
	data.set("data", PackedByteArray([1, 2, 3, 4]))
	data.set("baked_reflection_type", Constants.REFLECTION_TYPE_HYBRID)
	watch_signals(_logger)

	assert_eq(ResonanceServer.load_probe_batch(data), -1)
	await wait_process_frames(1)

	assert_signal_emitted(_logger, "log_entry_added")
	var params: Array = get_signal_parameters(_logger, "log_entry_added")
	assert_eq(params[0], &"validation")

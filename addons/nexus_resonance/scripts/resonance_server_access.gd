extends Object
class_name ResonanceServerAccess

## Typed helpers for the [ResonanceServer] GDExtension singleton ([method get_server] / [method get_server_if_initialized] → [Variant]).

## Engine singleton registration name.
const SINGLETON_NAME := &"ResonanceServer"


## Returns [code]true[/code] if the singleton is registered (extension loaded).
static func has_server() -> bool:
	return Engine.has_singleton(SINGLETON_NAME)


## Returns the native server instance, or [code]null[/code] if the singleton is missing.
static func get_server() -> Variant:
	if not has_server():
		return null
	return Engine.get_singleton(SINGLETON_NAME)


## Returns the server if it exists and reports initialized, else [code]null[/code].
static func get_server_if_initialized() -> Variant:
	var s: Variant = get_server()
	if s == null:
		return null
	if not s.has_method("is_initialized") or not s.is_initialized():
		return null
	return s

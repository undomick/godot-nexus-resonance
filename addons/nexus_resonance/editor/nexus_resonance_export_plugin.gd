@tool
extends EditorExportPlugin

## Adds Steam Audio runtime shared libraries to exports (same paths as [dependencies] in nexus_resonance.gdextension).
## Windows: DLLs next to the .exe (not resolved from .pck). Linux/macOS/Android: packaged like other native deps.

const _SUPPORTED_OS := ["Windows", "Linux", "macOS", "Android"]

## ABIs we ship libphonon.so for (must match bin/android/ layout).
const _ANDROID_STEAM_AUDIO: Dictionary = {
	"arm64-v8a": "res://addons/nexus_resonance/bin/android/arm64-v8a/libphonon.so",
	"x86_64": "res://addons/nexus_resonance/bin/android/x86_64/libphonon.so",
}

## Godot Android preset ABIs with no Steam Audio binary in this addon.
const _ANDROID_NO_STEAM_AUDIO_ABIS := ["armeabi-v7a", "x86"]


func _get_name() -> String:
	return "NexusResonanceSteamAudio"


func _supports_platform(platform: EditorExportPlatform) -> bool:
	return platform.get_os_name() in _SUPPORTED_OS


func _export_begin(
	_features: PackedStringArray, _is_debug: bool, _path: String, _flags: int
) -> void:
	var os_name: String = get_export_platform().get_os_name()
	match os_name:
		"Windows":
			for dll_path in [
				"res://addons/nexus_resonance/bin/windows/phonon.dll",
				"res://addons/nexus_resonance/bin/windows/GPUUtilities.dll",
				"res://addons/nexus_resonance/bin/windows/TrueAudioNext.dll",
			]:
				_add_if_exists(dll_path, PackedStringArray())
		"Linux":
			_add_if_exists(
				"res://addons/nexus_resonance/bin/linux/libphonon.so", PackedStringArray()
			)
		"macOS":
			_add_if_exists(
				"res://addons/nexus_resonance/bin/macos/libphonon.dylib", PackedStringArray()
			)
		"Android":
			_export_begin_android()
		_:
			pass


func _export_begin_android() -> void:
	for abi in _ANDROID_STEAM_AUDIO.keys():
		var opt := "architectures/%s" % abi
		if not bool(get_option(opt)):
			continue
		var lib_path: String = _ANDROID_STEAM_AUDIO[abi]
		_add_if_exists(lib_path, PackedStringArray([abi]))

	for abi in _ANDROID_NO_STEAM_AUDIO_ABIS:
		var opt := "architectures/%s" % abi
		if bool(get_option(opt)):
			push_warning(
				(
					"Nexus Resonance Export: Android ABI '%s' is enabled but this addon does not ship libphonon.so for it. "
					+ "Disable the ABI or provide a matching library." % abi
				)
			)


func _add_if_exists(res_path: String, tags: PackedStringArray) -> void:
	if FileAccess.file_exists(res_path):
		add_shared_object(res_path, tags, "")
	else:
		push_warning("Nexus Resonance Export: Native library not found: %s" % res_path)

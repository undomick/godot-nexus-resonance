@tool
@icon("res://addons/nexus_resonance/ui/icons/resonance_config.svg")
extends Resource
class_name ResonanceRuntimeConfig

## Runtime settings resource for [ResonanceRuntime]. Assign it on the runtime node or as a project default.
##
## Convolution/TAN reverb is mixed once on [member reverb_bus_name] by [ResonanceAudioEffect]; Parametric/Hybrid reverb is mixed per [ResonancePlayer]. The dry signal uses [member bus] unless a player overrides it.

const Constants = preload("resonance_config_constants.gd")

signal reflection_type_changed(new_type: int)
signal pathing_enabled_changed(enabled: bool)
## Emitted when the global bake Ambisonic order changes. Does not reinit the audio engine.
signal bake_ambisonic_order_changed(new_order: int)
## Emitted when a field changes that needs an engine reinit; [ResonanceRuntime] then reinits the audio engine.
signal native_engine_reinit_requested(property_name: StringName)
## Emitted when a field can be patched on the running server without reinit.
signal runtime_live_config_changed(property_name: StringName)
## Emitted when Godot bus routing fields change.
signal runtime_routing_changed(property_name: StringName)

var _suppress_runtime_config_notifications: bool = false

const _RUNTIME_CONFIG_CUSTOM_SETTER_PROPERTIES: Array[StringName] = [
	&"reflection_type",
	&"realtime_rays",
	&"pathing_enabled",
	&"direct_sim_interval",
	&"reflections_sim_interval",
	&"pathing_sim_interval",
	&"scene_type",
	&"realtime_reflection_max_distance",
]

@export_group("Output & Routing")
## Output bus for Direct + Pathing. Empty = Master.
@export var bus: StringName = ResonancePaths.DEFAULT_OUTPUT_BUS_NAME
## Bus that hosts [ResonanceAudioEffect] for Convolution/TAN reverb. Empty = ResonanceReverb. It sends to the effective [member bus].
@export var reverb_bus_name: StringName = ResonancePaths.DEFAULT_REVERB_BUS_NAME

@export_group("Spatialization")
## Use virtual surround instead of HRTF. Simpler, less accurate.
@export var use_virtual_surround: bool = false
## Direct path without HRTF: speaker layout for IPLPanningEffect (and optional Ambisonics -> speaker decode for surround).
## Godot player output stays stereo (fold-down). Use 1/2/4/6/8 only; other values become stereo at runtime.
@export_enum("Mono:1", "Stereo:2", "Quad:4", "5.1:6", "7.1:8") var direct_speaker_channels: int = 2

@export_subgroup("Headphone HRTF", "")
## Apply directional HRTF on the [b]direct[/b] dry path instead of speaker panning. Requires a loaded HRTF.
@export var direct_binaural: bool = true
## Apply HRTF when decoding convolution / mixer Ambisonics to stereo (wet / [member reverb_bus_name]).
@export var reverb_binaural: bool = true
## Apply HRTF in the pathing effect (indirect paths around obstacles).
@export var pathing_binaural: bool = true

@export_storage var _spatial_binaural_config_version: int = 0

@export_subgroup("HRTF", "")
## HRTF volume gain (dB) for the embedded default HRTF. With custom SOFA, added to each asset's [member ResonanceSOFAAsset.volume_db] (linear gain product).
@export_range(-24.0, 24.0, 0.1) var hrtf_volume_db: float = 0.0
## HRTF normalization for the embedded default HRTF only (None / RMS). Custom SOFA files use [member ResonanceSOFAAsset.norm_type] on the asset.
@export_enum("None:0", "RMS:1") var hrtf_normalization_type: int = 0
## SOFA HRTF library. [member hrtf_sofa_selected_index] picks the active entry. Empty = default embedded HRTF.
@export var hrtf_sofa_assets: Array[ResonanceSOFAAsset] = []
## Index into [member hrtf_sofa_assets] for the active SOFA (clamped at runtime).
@export_range(0, 64, 1) var hrtf_sofa_selected_index: int = 0
## Bilinear HRTF interpolation. Smoother than nearest, slightly more CPU.
@export var hrtf_interpolation_bilinear: bool = false

@export_subgroup("Third-person Perspective", "")
## Enable perspective correction for spatialized sound in third-person. Patches the running server at runtime (no reinit).
@export var perspective_correction_enabled: bool = false
## Perspective correction factor. 1.0 = calibrated for a 30-32 inch desktop monitor. Live-patched with [member perspective_correction_enabled].
@export_range(0.5, 2.0, 0.1) var perspective_correction_factor: float = 1.0

@export_group("Reflections & Reverb")
var _reflection_type: int = Constants.REFLECTION_TYPE_CONVOLUTION
## Reverb algorithm. Parametric is the fastest. Hybrid = convolution plus parametric tail and can be slower than Convolution;
## lower [member hybrid_reverb_transition_time] and [member realtime_ambisonic_order] to speed it up.
## TrueAudio Next works on 64-bit Windows only; other platforms fall back to Convolution with a warning.
@export_enum("Convolution:0", "Parametric:1", "Hybrid:2", "TrueAudio Next (AMD GPU):3")
var reflection_type: int:
	get:
		return _reflection_type
	set(v):
		if _reflection_type != v:
			_reflection_type = v
			reflection_type_changed.emit(v)
			notify_property_list_changed()
			_notify_runtime_config_property_changed(&"reflection_type")
var _ambisonic_order: int = 1
## Realtime Ambisonic Order for reverb playback (convolution channels, mixer/decode, pathing order)
## and realtime reflection simulation ([code]IPLSimulationSettings.maxOrder[/code]). Not the bake order.
@export_enum("1st Order:1", "2nd Order:2", "3rd Order:3") var realtime_ambisonic_order: int:
	get:
		return _ambisonic_order
	set(v):
		var clamped := clampi(int(v), 1, 3)
		if _ambisonic_order != clamped:
			_ambisonic_order = clamped
			_notify_runtime_config_property_changed(&"ambisonic_order")
## Script / C++ / legacy .tres alias for [member realtime_ambisonic_order] (engine dict key [code]ambisonic_order[/code]).
var ambisonic_order: int:
	get:
		return _ambisonic_order
	set(v):
		realtime_ambisonic_order = v
var _bake_ambisonic_order: int = 1
## Bake Ambisonic Order for probe reflection IRs. Volumes can override it with [member ResonanceBakeConfig.bake_ambisonics_order].
@export_enum("1st Order:1", "2nd Order:2", "3rd Order:3") var bake_ambisonic_order: int:
	get:
		return _bake_ambisonic_order
	set(v):
		var clamped := clampi(int(v), 1, 3)
		if _bake_ambisonic_order != clamped:
			_bake_ambisonic_order = clamped
			bake_ambisonic_order_changed.emit(_bake_ambisonic_order)
var _realtime_rays: int = 0
## Ray count for realtime reflection simulation. Off disables realtime reflections (cheapest).
## Higher values improve quality but cost more CPU; depends strongly on [member scene_type].
@export_enum(
	"Off:0",
	"8 Rays:8",
	"16 Rays:16",
	"32 Rays:32",
	"64 Rays:64",
	"128 Rays:128",
	"256 Rays:256",
	"512 Rays:512",
	"1024 Rays:1024",
	"2048 Rays:2048",
	"4096 Rays:4096",
	"8192 Rays:8192"
)
var realtime_rays: int:
	get:
		return _realtime_rays
	set(v):
		if _realtime_rays != v:
			_realtime_rays = v
			notify_property_list_changed()
			_notify_runtime_config_property_changed(&"realtime_rays")
## Min distance for irradiance sampling. Lower = finer detail at close range, more CPU. Only when Realtime Rays > 0.
@export_range(0.05, 10.0, 0.01) var realtime_irradiance_min_distance: float = 0.1
## Diffuse samples per reflection point. Higher = smoother reverb, more CPU. Only when Realtime Rays > 0.
@export_range(8, 64, 1) var realtime_num_diffuse_samples: int = 32
## Realtime reflection bounces per ray. Higher = longer reverb, more CPU.
@export_range(1, 64, 1) var realtime_bounces: int = 4
## Impulse response length in seconds, used for simulation, effect buffers and the end-of-stream tail. Longer costs more CPU and memory.
@export_range(0.1, 10.0, 0.1) var realtime_simulation_duration: float = 2.0
## Hybrid reverb: seconds of IR rendered by convolution before the parametric tail. Lower = less CPU, e.g. 0.2-0.3 s. Only when reflection_type is Hybrid.
@export_range(0.1, 2.0, 0.1) var hybrid_reverb_transition_time: float = 1.0
## Hybrid reverb: crossfade overlap in percent between the convolution and parametric parts.
@export_range(0, 100, 1) var hybrid_reverb_overlap_percent: int = 25

@export_group("Pathing")
var _pathing_enabled: bool = false
## Enable pathing (multi-path sound propagation). Requires baked pathing in Probe Volumes.
## Live-togglable when the simulator was created with pathing; enabling after a pathing-off start reinits the engine.
@export var pathing_enabled: bool:
	get:
		return _pathing_enabled
	set(v):
		if _pathing_enabled != v:
			_pathing_enabled = v
			pathing_enabled_changed.emit(v)
			notify_property_list_changed()
			_notify_runtime_config_property_changed(&"pathing_enabled")
## Pathing: normalize EQ on path effect output. Prevents pathing from sounding too bright. Live-patchable.
@export var pathing_normalize_eq: bool = false
## Pathing visibility samples for bake and runtime. With path validation or alternate paths on, this dominates pathing cost.
## Changing it at runtime reinits the audio engine.
@export_range(1, 16, 1) var pathing_num_samples: int = 4
## Baking Visibility Range (m). Probes farther apart are not mutually visible. Used for bake and runtime path validation.
@export_range(0.0, 1000.0, 1.0) var pathing_vis_range: float = 1000.0
## Baking Path Range (m). Max path length between probes. Bake only.
@export_range(0.0, 1000.0, 1.0) var pathing_path_range: float = 1000.0
## Baking Visibility Radius (m). Probe sphere radius for visibility tests at bake and runtime.
@export_range(0.0, 2.0, 0.1) var pathing_vis_radius: float = 1.0
## Baking Visibility Threshold. Fraction of unoccluded rays required to link probes, at bake and runtime.
@export_range(0.0, 1.0, 0.01) var pathing_vis_threshold: float = 0.1
## Default when a [ResonancePlayer] uses [b]Use Global[/b] for path validation: re-check baked paths for occlusion by dynamic geometry each update (higher CPU). Live-patchable.
@export var path_validation_enabled: bool = true
## Default when a player uses [b]Use Global[/b] for alternate paths: search realtime routes when a baked path is occluded.
## Very CPU-heavy; only effective if validation is enabled. Live-patchable.
@export var find_alternate_paths: bool = false

@export_group("Occlusion & Transmission")
## Occlusion model: Raycast (binary hit) or Volumetric (fractional occlusion from samples). Does not change transmission.
## Volumetric uses [member ResonancePlayerConfig.occlusion_samples] and [member ResonancePlayerConfig.occlusion_radius] of each source.
@export_enum("Raycast:0", "Volumetric:1") var occlusion_type: int = 1
## Simulator cap for volumetric occlusion samples (Steam Audio [code]maxNumOcclusionSamples[/code]). Per-source [member ResonancePlayerConfig.occlusion_samples] are clamped to this.
@export_range(1, 128, 1) var max_occlusion_samples: int = 64
## Transmission on the direct effect: FreqIndependent = one blended coefficient, FreqDependent = low/mid/high bands.
@export_enum("FreqIndependent:0", "FreqDependent:1") var transmission_type: int = 1
## Max surfaces along the transmission path, closest to the listener first. [code]1[/code] = nearest surface only.
## Each further hit multiplies the loss and can silence transmission quickly. Used for players set to Use Global.
@export_range(1, 256, 1) var max_transmission_surfaces: int = 1

@export_group("Performance & Scheduling")
var _performance_schedule_selector: int = 0
var _applying_performance_schedule_preset: bool = false

## Preset for [member reflections_sim_interval], [member pathing_sim_interval] and [member direct_sim_interval].
## Switches back to Custom when you change one of them by hand.
@export_enum("Custom:0", "Quality:1", "Balanced:2", "Performance:3")
var apply_performance_schedule_preset: int = 0:
	get:
		return _performance_schedule_selector
	set(v):
		_performance_schedule_selector = v
		if v == 0:
			notify_property_list_changed()
			return
		_applying_performance_schedule_preset = true
		match v:
			1:
				reflections_sim_interval = 0.1
				pathing_sim_interval = 0.1
				direct_sim_interval = 0.0
			2:
				reflections_sim_interval = 0.2
				pathing_sim_interval = 0.2
				direct_sim_interval = 0.03
			3:
				reflections_sim_interval = 0.3
				pathing_sim_interval = 0.3
				direct_sim_interval = 0.1
		_applying_performance_schedule_preset = false
		notify_property_list_changed()
		_emit_runtime_live_config_after_performance_preset()

## Fraction of logical CPU cores for Steam Audio simulation threads. Raise it for heavy realtime reflections or pathing.
@export_range(0.0, 1.0, 0.01) var simulation_cpu_cores_percent: float = 0.15
var _direct_sim_interval: float = 0.0
## Minimum seconds between direct simulation runs (occlusion, transmission, air absorption), independent of the reflection and pathing intervals.
## [code]0[/code] = every worker wake. Try [code]0.02[/code]-[code]0.05[/code] to lower CPU; occlusion then updates slightly less often.
@export_range(0.0, 1.0, 0.005) var direct_sim_interval: float:
	get:
		return _direct_sim_interval
	set(v):
		if _direct_sim_interval == v:
			return
		_direct_sim_interval = v
		_on_performance_knob_changed()
		_notify_runtime_config_property_changed(&"direct_sim_interval")
var _reflections_sim_interval: float = 0.1
## Minimum seconds between reflection simulation runs. [code]0[/code] = every worker tick (highest CPU). Direct occlusion and transmission use [member direct_sim_interval].
@export_range(0.0, 1.0, 0.01) var reflections_sim_interval: float:
	get:
		return _reflections_sim_interval
	set(v):
		if _reflections_sim_interval == v:
			return
		_reflections_sim_interval = v
		_on_performance_knob_changed()
		_notify_runtime_config_property_changed(&"reflections_sim_interval")
var _pathing_sim_interval: float = 0.1
## Minimum seconds between pathing simulation runs. Set it higher than [member reflections_sim_interval] to stagger the two.
@export_range(0.0, 1.0, 0.01) var pathing_sim_interval: float:
	get:
		return _pathing_sim_interval
	set(v):
		if _pathing_sim_interval == v:
			return
		_pathing_sim_interval = v
		_on_performance_knob_changed()
		_notify_runtime_config_property_changed(&"pathing_sim_interval")
## Maximum simultaneous sources for realtime reflection simulation (Steam Audio [code]maxNumSources[/code]). Higher values use more CPU and memory.
@export_range(8, 128, 1) var max_simulation_sources: int = 32
## Minimum seconds between applying queued dynamic geometry transforms to Steam Audio. Limits scene commit cost.
@export_range(0.0, 1.0, 0.005) var dynamic_scene_commit_min_interval: float = 0.0
## When on, players enqueue source updates and ResonanceRuntime applies them once per frame (reduces lock contention).
@export var batch_source_updates: bool = true

@export_group("Scene Backend & Physics")
var _scene_type: int = 0
## Ray tracer backend. Default = built-in Steam Audio; Embree is often faster on CPU. Radeon Rays (OpenCL GPU) works on 64-bit Windows only,
## elsewhere it falls back to Default. Custom = rays use Godot 3D physics and simulation runs in the physics frame.
@export_enum("Default:0", "Embree:1", "Radeon Rays:2", "Custom (Godot Physics):3")
var scene_type: int:
	get:
		return _scene_type
	set(v):
		if _scene_type != v:
			_scene_type = v
			notify_property_list_changed()
			_notify_runtime_config_property_changed(&"scene_type")
## Collision mask for Godot [code]PhysicsRayQueryParameters3D[/code] when [member scene_type] is Custom. [code]-1[/code] = all physics layers.
@export_flags_3d_physics var physics_ray_collision_mask: int = -1
## Rays per Steam Audio job when [member scene_type] is Custom. Values > 1 enable batched Godot ray callbacks.
@export_range(1, 256, 1) var physics_ray_batch_size: int = 16
## OpenCL device type when scene_type is Radeon Rays (or when using TAN). Helps when a GPU has OpenCL issues.
@export_enum("GPU:0", "CPU:1", "Any:2") var opencl_device_type: int = 0
## OpenCL device index (0 = first matching device). Useful when multiple GPUs are present.
@export_range(0, 31, 1) var opencl_device_index: int = 0

@export_group("Expert")
## Adaptive scheduling: when the last reflections tick exceeds this many microseconds, increase the effective reflection interval.
## 0 = off.
@export_range(0, 2000000, 1000) var reflections_adaptive_budget_us: int = 0
## Lower bound for adaptive realtime [code]numRays[/code] when [member reflections_adaptive_budget_us] > 0.
@export_range(32, 65535, 1) var reflections_adaptive_ray_min: int = 128
## How fast adaptive rays recover toward [member realtime_rays] when under budget (fraction of max rays per reflections tick).
@export_range(0.0, 1.0, 0.005) var reflections_adaptive_ray_recover_frac: float = 0.125
## Cap for per-tick ray recovery (0 = unlimited; useful to prevent big jumps after a long under-budget stretch).
@export_range(0, 65535, 32) var reflections_adaptive_ray_recover_cap: int = 512
## Seconds added to the effective reflection interval each time the worker exceeds [member reflections_adaptive_budget_us].
@export_range(0.0, 1.0, 0.005) var reflections_adaptive_step_sec: float = 0.02
## Upper bound (seconds) for extra delay from adaptive reflection scheduling.
@export_range(0.0, 1.0, 0.01) var reflections_adaptive_max_extra_interval: float = 0.2
## Per-second reduction of adaptive extra delay when under budget (or worker did not run reflections).
@export_range(0.0, 5.0, 0.01) var reflections_adaptive_decay_per_sec: float = 0.05
## When [code]iplSceneCommit[/code] took at least this many microseconds, skip reflections this wake and retry next frame.
## 0 = off.
@export_range(0, 5000000, 1000) var reflections_defer_after_scene_commit_us: int = 0
## Convolution / hybrid / TAN apply path: clamp IR length to this many samples (min with allocated effect IR).
## 0 = no cap.
@export_range(0, 480000, 256) var convolution_ir_max_samples: int = 0
var _realtime_reflection_max_distance_m: float = 0.0
## Max distance for realtime reflections (meters). 0 disables the distance cull.
## Sources farther than this omit reflections simulation flags (cheaper [code]RunReflections[/code]).
@export_range(0.0, 10000.0, 1.0) var realtime_reflection_max_distance: float:
	get:
		return _realtime_reflection_max_distance_m
	set(v):
		if _realtime_reflection_max_distance_m == v:
			return
		_realtime_reflection_max_distance_m = v
		_notify_runtime_config_property_changed(&"realtime_reflection_max_distance")


func _get(property: StringName) -> Variant:
	# Legacy .tres files and C++ read ambisonic_order through Object.get().
	if property == &"ambisonic_order":
		return _ambisonic_order
	return null


func _set(property: StringName, value: Variant) -> bool:
	# Old resources serialized ambisonic_order instead of realtime_ambisonic_order.
	if property == &"ambisonic_order":
		realtime_ambisonic_order = int(value)
		return true
	return false


func _validate_property(property: Dictionary) -> void:
	if property.name == "ambisonic_order":
		# The inspector shows realtime_ambisonic_order; this alias is only stored.
		property["usage"] = PROPERTY_USAGE_STORAGE
	elif property.name in ["hybrid_reverb_transition_time", "hybrid_reverb_overlap_percent"]:
		if reflection_type != Constants.REFLECTION_TYPE_HYBRID:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name in ["opencl_device_type", "opencl_device_index"]:
		if scene_type != 2 and reflection_type != Constants.REFLECTION_TYPE_TAN:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "physics_ray_batch_size":
		if scene_type != 3:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif (
		property.name
		in [
			"realtime_irradiance_min_distance",
			"realtime_num_diffuse_samples",
			"realtime_bounces",
			"realtime_simulation_duration"
		]
	):
		if realtime_rays == 0:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif (
		property.name
		in [
			"pathing_normalize_eq",
			"pathing_num_samples",
			"pathing_vis_range",
			"pathing_path_range",
			"pathing_vis_radius",
			"pathing_vis_threshold",
			"path_validation_enabled",
			"find_alternate_paths"
		]
	):
		if not pathing_enabled:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY


func _on_performance_knob_changed() -> void:
	if _applying_performance_schedule_preset:
		return
	if _performance_schedule_selector != 0:
		_performance_schedule_selector = 0
		notify_property_list_changed()


## Returns effective bus for Direct + Pathing. Empty config = Master.
func get_bus_effective() -> StringName:
	return bus if not bus.is_empty() else ResonancePaths.DEFAULT_OUTPUT_BUS_NAME


## Returns effective reverb bus name. Empty config = ResonanceReverb.
func get_reverb_bus_name_effective() -> StringName:
	return (
		reverb_bus_name
		if not reverb_bus_name.is_empty()
		else ResonancePaths.DEFAULT_REVERB_BUS_NAME
	)


func _sofa_asset_data_nonempty(asset: ResonanceSOFAAsset) -> bool:
	return asset != null and not asset.get_sofa_data().is_empty()


## The SOFA at [member hrtf_sofa_selected_index], else the first entry with data. Null if no entry has SOFA data.
func get_hrtf_sofa_effective() -> ResonanceSOFAAsset:
	if hrtf_sofa_assets.is_empty():
		return null
	var idx := clampi(hrtf_sofa_selected_index, 0, hrtf_sofa_assets.size() - 1)
	var picked: ResonanceSOFAAsset = hrtf_sofa_assets[idx]
	if _sofa_asset_data_nonempty(picked):
		return picked
	for i in hrtf_sofa_assets.size():
		var a: ResonanceSOFAAsset = hrtf_sofa_assets[i]
		if _sofa_asset_data_nonempty(a):
			return a
	return null


## Returns [param p_realtime_rays] unchanged. Pass [method OS.get_name] as the OS name; it is reserved for per-OS caps.
static func get_effective_realtime_rays(p_realtime_rays: int, _os_name: String) -> int:
	return p_realtime_rays


## Godot mix buffer size derived from audio/driver/output_latency. Matches the reverb bus frame_count.
static func _get_audio_frame_size_from_project() -> int:
	var lat_ms: float = 15.0
	if ProjectSettings.has_setting("audio/driver/output_latency"):
		var lat_var = ProjectSettings.get_setting("audio/driver/output_latency")
		if lat_var is float:
			lat_ms = lat_var
		elif lat_var is int:
			lat_ms = float(lat_var)
	var mix_rate := int(AudioServer.get_mix_rate())
	var raw := int(lat_ms * mix_rate / 1000.0)
	# Godot's mix buffer is one of these sizes.
	var candidates := [256, 512, 1024, 2048]
	var best := 512
	var best_dist := 999999
	for c in candidates:
		var d := absi(raw - c)
		if d < best_dist:
			best_dist = d
			best = c
	return best


func _migrate_spatial_binaural_if_needed() -> void:
	if _spatial_binaural_config_version >= 2:
		return
	if resource_path.is_empty():
		_spatial_binaural_config_version = 2
		return
	_suppress_runtime_config_notifications = true
	direct_binaural = reverb_binaural
	pathing_binaural = reverb_binaural
	_suppress_runtime_config_notifications = false
	_spatial_binaural_config_version = 2
	emit_changed()


func _notify_runtime_config_property_changed(property: StringName) -> void:
	if _suppress_runtime_config_notifications or _applying_performance_schedule_preset:
		return
	if not ClassDB.class_exists("ResonanceServer"):
		return
	if ResonanceServer.runtime_config_property_requires_engine_reinit(property):
		native_engine_reinit_requested.emit(property)
	elif ResonanceServer.runtime_config_property_is_live_patchable(property):
		runtime_live_config_changed.emit(property)
	elif ResonanceServer.runtime_config_property_requires_routing_refresh(property):
		runtime_routing_changed.emit(property)


func _emit_runtime_live_config_after_performance_preset() -> void:
	# One notify covers all three intervals: the live apply reads the whole config.
	_notify_runtime_config_property_changed(&"reflections_sim_interval")


@warning_ignore("native_method_override")
func set(property: StringName, value: Variant) -> void:
	if _suppress_runtime_config_notifications:
		super.set(property, value)
		return
	if not ClassDB.class_exists("ResonanceServer"):
		super.set(property, value)
		return
	var tracked := (
		ResonanceServer.runtime_config_property_requires_engine_reinit(property)
		or ResonanceServer.runtime_config_property_is_live_patchable(property)
		or ResonanceServer.runtime_config_property_requires_routing_refresh(property)
	)
	if _RUNTIME_CONFIG_CUSTOM_SETTER_PROPERTIES.has(property):
		super.set(property, value)
		return
	if not tracked:
		super.set(property, value)
		return
	var prev = get(property)
	super.set(property, value)
	if prev != value:
		_notify_runtime_config_property_changed(property)


## Engine config for [method ResonanceServer.init_audio_engine]. Bus names are not included; [method ResonanceRuntime.get_config_dict] adds the context settings.
func get_config() -> Dictionary:
	_migrate_spatial_binaural_if_needed()
	var rays := get_effective_realtime_rays(realtime_rays, OS.get_name())
	# Host mixer only: no sample-rate or frame-size overrides.
	return {
		"sample_rate": int(AudioServer.get_mix_rate()),
		"audio_frame_size": _get_audio_frame_size_from_project(),
		"audio_frame_size_was_auto": true,
		"ambisonic_order": ambisonic_order,
		"simulation_cpu_cores_percent": simulation_cpu_cores_percent,
		"realtime_rays": rays,
		"realtime_bounces": realtime_bounces,
		"scene_type": scene_type,
		"physics_ray_collision_mask": physics_ray_collision_mask,
		"physics_ray_batch_size": physics_ray_batch_size,
		"opencl_device_type": opencl_device_type,
		"opencl_device_index": opencl_device_index,
		"realtime_irradiance_min_distance": realtime_irradiance_min_distance,
		"realtime_simulation_duration": realtime_simulation_duration,
		"realtime_num_diffuse_samples": realtime_num_diffuse_samples,
		"reflection_type": reflection_type,
		"hybrid_reverb_transition_time": hybrid_reverb_transition_time,
		"hybrid_reverb_overlap_percent": hybrid_reverb_overlap_percent,
		"direct_binaural": direct_binaural,
		"reverb_binaural": reverb_binaural,
		"pathing_binaural": pathing_binaural,
		"use_virtual_surround": use_virtual_surround,
		"direct_speaker_channels": direct_speaker_channels,
		"hrtf_volume_db": hrtf_volume_db,
		"hrtf_normalization_type": hrtf_normalization_type,
		"hrtf_sofa_asset": get_hrtf_sofa_effective(),
		"hrtf_interpolation_bilinear": hrtf_interpolation_bilinear,
		# Baked REVERB always uses listener-centric probe lookup.
		"baked_reverb_use_listener_probe": true,
		"pathing_enabled": pathing_enabled,
		"pathing_normalize_eq": pathing_normalize_eq,
		"pathing_num_samples": pathing_num_samples,
		"pathing_vis_range": pathing_vis_range,
		"pathing_vis_radius": pathing_vis_radius,
		"pathing_vis_threshold": pathing_vis_threshold,
		"path_validation_enabled": path_validation_enabled,
		"find_alternate_paths": find_alternate_paths,
		"transmission_type": transmission_type,
		"max_transmission_surfaces": max_transmission_surfaces,
		"occlusion_type": occlusion_type,
		"max_occlusion_samples": max_occlusion_samples,
		"max_simulation_sources": max_simulation_sources,
		"dynamic_scene_commit_min_interval": dynamic_scene_commit_min_interval,
		"reflections_sim_interval": reflections_sim_interval,
		"pathing_sim_interval": pathing_sim_interval,
		"realtime_reflection_max_distance_m": _realtime_reflection_max_distance_m,
		"reflections_adaptive_budget_us": reflections_adaptive_budget_us,
		"reflections_adaptive_ray_min": reflections_adaptive_ray_min,
		"reflections_adaptive_ray_recover_frac": reflections_adaptive_ray_recover_frac,
		"reflections_adaptive_ray_recover_cap": reflections_adaptive_ray_recover_cap,
		"reflections_adaptive_step_sec": reflections_adaptive_step_sec,
		"reflections_adaptive_max_extra_interval": reflections_adaptive_max_extra_interval,
		"reflections_adaptive_decay_per_sec": reflections_adaptive_decay_per_sec,
		"reflections_defer_after_scene_commit_us": reflections_defer_after_scene_commit_us,
		"convolution_ir_max_samples": convolution_ir_max_samples,
		"direct_sim_interval": direct_sim_interval,
		"batch_source_updates": batch_source_updates,
		"perspective_correction_enabled": perspective_correction_enabled,
		"perspective_correction_factor": perspective_correction_factor,
		"output_direct": true,
		"output_reverb": true
	}


## Default runtime config for the editor or when the scene has no ResonanceRuntime.
static func create_default() -> ResonanceRuntimeConfig:
	return ResonanceRuntimeConfig.new()

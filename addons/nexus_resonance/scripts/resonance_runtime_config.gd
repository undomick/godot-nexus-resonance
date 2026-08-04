@tool
@icon("res://addons/nexus_resonance/ui/icons/resonance_config.svg")
extends Resource
class_name ResonanceRuntimeConfig

## Runtime settings resource for [ResonanceRuntime]. Steam Audio–oriented fields; assign on the runtime node or as a project default.
##
## Routing sketch: Convolution/TAN use a shared wet path on [member reverb_bus_name] ([ResonanceAudioEffect]); parametric/hybrid wet is mixed in [ResonancePlayer] with optional split to [method ResonancePlayerConfig.get_reverb_bus_name_effective]. Dry uses [member bus] and per-player overrides.

const Constants = preload("resonance_config_constants.gd")

signal reflection_type_changed(new_type: int)
signal pathing_enabled_changed(enabled: bool)
signal audio_frame_size_changed(new_size: int)

# --- Output & Routing ---
@export_group("Output & Routing")
## Output bus for Direct + Pathing. Empty = Master.
@export var bus: StringName = ResonancePaths.DEFAULT_OUTPUT_BUS_NAME
## Bus that hosts [ResonanceAudioEffect] (convolution / TAN wet) and the reverb activator. Empty = ResonanceReverb. Godot send for this bus follows [method get_bus_effective] (same as Direct+Pathing runtime bus).
@export var reverb_bus_name: StringName = ResonancePaths.DEFAULT_REVERB_BUS_NAME

# --- Audio Engine ---
@export_group("Audio Engine")
## Sample rate override. Use Godot Mix Rate to follow Project Settings.
## Only override when you know the whole project runs at that rate; there is no resampling and mismatches can cause artifacts.
@export_enum(
	"Use Godot Mix Rate:0",
	"22050 Hz:22050",
	"44100 Hz:44100",
	"48000 Hz:48000",
	"96000 Hz:96000",
	"192000 Hz:192000"
)
var sample_rate_override: int = 0
## Steam Audio processing block size in samples per channel per [code]iplAudioEffectApply[/code] call.
## For stable routing it should match Godot's mix buffer size (reverb bus frame_count).
## Auto picks the closest of 256/512/1024/2048 from [code]audio/driver/output_latency[/code] and the current mix rate.
var _audio_frame_size: int = 0
@export_enum("Auto:0", "256:256", "512:512", "1024:1024", "2048:2048")
## Audio processing block size (samples per channel per apply call).
## Auto is recommended. Use Manual only when you're tuning latency vs CPU and know your mix buffer size.
var audio_frame_size: int:
	get:
		return _audio_frame_size
	set(v):
		if _audio_frame_size != v:
			_audio_frame_size = v
			audio_frame_size_changed.emit(v)

# --- HRTF & Spatialization ---
@export_group("Spatialization")
## Use virtual surround instead of HRTF. Simpler, less accurate.
@export var use_virtual_surround: bool = false
## Direct path without HRTF: speaker layout for IPLPanningEffect (and optional Ambisonics→speaker decode for surround).
## Godot player output stays stereo (fold-down). Use 1/2/4/6/8 only; other values become stereo at runtime.
@export_enum("Mono:1", "Stereo:2", "Quad:4", "5.1:6", "7.1:8") var direct_speaker_channels: int = 2

@export_subgroup("Headphone HRTF", "")
## Apply directional HRTF on the **direct** dry path (vs speaker panning). Requires a loaded HRTF.
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
## Enable perspective correction for spatialized sound in third-person.
@export var perspective_correction_enabled: bool = false
## Perspective correction factor. 1.0 = calibrated for a 30–32 inch desktop monitor.
@export_range(0.5, 2.0, 0.1) var perspective_correction_factor: float = 1.0

# --- Reflections & Reverb ---
@export_group("Reflections & Reverb")
## Default reflections mode for sources that use **Use Global**.
## **Baked** = probe reverbs (stored per probe; not the same as ray-traced occlusion through walls).
## **Realtime** = ray-traced simulation against the acoustic scene (requires [member realtime_rays] > 0) - use for sealed rooms / geometry-dependent indirect sound.
@export_enum("Baked:0", "Realtime:1") var default_reflections_mode: int = 0
var _reflection_type: int = Constants.REFLECTION_TYPE_CONVOLUTION
## Reverb algorithm. Parametric (fastest). Convolution uses ReflectionMixer (bundled convolutions).
## Hybrid = convolution + parametric tail (no mixer; can be slower than Convolution – reduce hybrid_reverb_transition_time and ambisonic_order for better perf).
## TrueAudio Next: Steam Audio supports TAN on 64-bit Windows only; other platforms fall back to Convolution with a warning.
@export_enum("Convolution:0", "Parametric:1", "Hybrid:2", "TrueAudio Next (AMD GPU):3")
var reflection_type: int:
	get:
		return _reflection_type
	set(v):
		if _reflection_type != v:
			_reflection_type = v
			reflection_type_changed.emit(v)
			notify_property_list_changed()
## Ambisonic order for reverb playback (convolution effect channels, mixer/decode, pathing order)
## and for realtime reflection simulation ([code]IPLSimulationSettings.maxOrder[/code]).
@export_enum("1st Order:1", "2nd Order:2", "3rd Order:3") var ambisonic_order: int = 1
## Upper bound for reverb IR allocation (seconds). Higher values increase memory/CPU.
## This is an allocation/mixer cap, not the per-tick realtime simulation length (see [member realtime_simulation_duration]).
@export_range(0.1, 10.0, 0.1) var max_reverb_duration: float = 2.0
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
## Min distance for irradiance sampling. Lower = finer detail at close range, more CPU. Only when Realtime Rays > 0.
@export_range(0.05, 10.0, 0.01) var realtime_irradiance_min_distance: float = 0.1
## Diffuse samples per reflection point. Higher = smoother reverb, more CPU. Only when Realtime Rays > 0.
@export_range(8, 64, 1) var realtime_num_diffuse_samples: int = 32
## Realtime reflection bounces per ray. Higher = longer reverb, more CPU.
@export_range(1, 64, 1) var realtime_bounces: int = 4
## Impulse response length (seconds) simulated each realtime tick. Longer increases CPU/memory.
## Distinct from [member max_reverb_duration] (allocation cap).
@export_range(0.1, 10.0, 0.1) var realtime_simulation_duration: float = 2.0
## Hybrid reverb: length (seconds) of IR used for convolution before parametric tail. Lower = less CPU (e.g. 0.2–0.3 s for better hybrid performance). Only when reflection_type is Hybrid.
@export_range(0.1, 2.0, 0.1) var hybrid_reverb_transition_time: float = 1.0
## Hybrid reverb: overlap percent (0–100) for crossfade between convolution and parametric parts.
@export_range(0, 100, 1) var hybrid_reverb_overlap_percent: int = 25

# --- Baked Reverb & Pathing ---
@export_group("Baked Reverb & Pathing")
## Probes beyond this radius (m) do not affect listener.
@export var reverb_influence_radius: float = 10000.0
## [b]Reflections sampling mode[/b]. This controls where baked REVERB chooses its probe from.
## [br][b]Listener-centric[/b] = pick the probe nearest the listener (recommended for room reverb).
## [br][b]Source-centric[/b] = pick the probe nearest the source (legacy behavior).
## [br][br]Note: currently this only affects baked REVERB probe lookup. Steam Audio realtime reflections trace rays from the listener in the core API, so this mode does not yet change realtime ray origin.
@export_enum("Listener-centric:0", "Source-centric:1") var reflections_sampling_mode: int = 0
var _pathing_enabled: bool = false
## Enable pathing (multi-path sound propagation). Requires baked pathing in Probe Volumes.
@export var pathing_enabled: bool:
	get:
		return _pathing_enabled
	set(v):
		if _pathing_enabled != v:
			_pathing_enabled = v
			pathing_enabled_changed.emit(v)
			notify_property_list_changed()
## Pathing: normalize EQ on path effect output. Prevents pathing from sounding too bright.
@export var pathing_normalize_eq: bool = true
## Runtime pathing: Steam Audio [code]numVisSamples[/code] (probe visibility rays per pathing update). 1–16. Lower = less CPU; higher ≈ closer to bake density ([ResonanceBakeConfig] [code]bake_pathing_num_samples[/code] is bake-only). With path validation / alternate paths on, this dominates pathing cost (Embree or Default tracer).
@export_range(1, 16, 1) var pathing_num_vis_samples: int = 4
## Default when a [ResonancePlayer] uses **Use Global** for path validation: re-check baked paths for occlusion by dynamic geometry each update (higher CPU).
@export var path_validation_enabled: bool = true
## Default when a player uses **Use Global** for alternate paths: search realtime routes when a baked path is occluded.
## Very CPU-heavy; only effective if validation is enabled.
@export var find_alternate_paths: bool = false
## How much transmission damps reverb (0–1). 0 = no damping. 1 = full damping (reverb scaled by wall transparency).
## Only consulted for baked REVERB paths (see [member apply_occlusion_to_baked_reflections]); realtime reflections and
## STATICSOURCE/STATICLISTENER already encode source→listener occlusion in the IR.
@export_range(0.0, 1.0, 0.01) var reverb_transmission_amount: float = 1.0
## Baked REVERB: multiply wet by direct-path occlusion/transmission. Default [code]false[/code] - LOS occlusion does not separate “same room, blocked sight” from “sealed source”; enabling dampens both and can kill plausible diffraction. Prefer realtime reflections or STATICSOURCE bakes for hard cases; see [code]docs/baked-reflections-and-outdoor-sources.md[/code].
@export var apply_occlusion_to_baked_reflections: bool = false
# --- Occlusion & Transmission ---
@export_group("Occlusion & Transmission")
## Occlusion model: Raycast (binary hit) or Volumetric (fractional occlusion from samples; Steam Audio [code]numOcclusionSamples[/code]). Volumetric only affects how occlusion is computed, not how transmission coefficients are banded. Pair with [member ResonancePlayerConfig.occlusion_samples] and [member ResonancePlayerConfig.source_radius] on each source.
@export_enum("Raycast:0", "Volumetric:1") var occlusion_type: int = 1
## Simulator cap for volumetric occlusion samples (Steam Audio [code]maxNumOcclusionSamples[/code]). Per-source [member ResonancePlayerConfig.occlusion_samples] are clamped to this.
@export_range(1, 128, 1) var max_occlusion_samples: int = 64
## Direct-effect frequency mode for transmission (Steam Audio [code]IPLTransmissionType[/code] on the direct processor): FreqIndependent (one blended coefficient) or FreqDependent (low/mid/high). This does not add “softer” material boundaries or a volumetric transmission path; Steam Audio [code]IPLSimulationInputs[/code] exposes [code]numTransmissionRays[/code] for path depth only, not an occlusion-style Raycast/Volumetric switch for transmission.
@export_enum("FreqIndependent:0", "FreqDependent:1") var transmission_type: int = 1
## Default max surfaces along the transmission path ([code]numTransmissionRays[/code]) when a [ResonancePlayerConfig] omits [member ResonancePlayerConfig.max_transmission_surfaces] or for initial simulator source state. Same range as per-source (1–256). Matches [member ResonancePlayerConfig.max_transmission_surfaces] default.
@export_range(1, 256, 1) var max_transmission_surfaces: int = 16
# --- Performance & Scheduling ---
@export_group("Performance & Scheduling")
var _performance_schedule_selector: int = 0
var _applying_performance_schedule_preset: bool = false

## Preset for a few key scheduling knobs ([member reflections_sim_interval], [member pathing_sim_interval], [member direct_sim_interval]).
## The selected value is kept until you manually tweak one of those knobs, then it switches back to Custom.
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

## Fraction of CPU cores for Steam Audio simulation threads (0–1). 0.15 ≈ 15% of logical cores; raise for heavier realtime reflections/pathing.
@export_range(0.0, 1.0, 0.01) var simulation_cpu_cores_percent: float = 0.15
var _direct_sim_interval: float = 0.0
## [b]Direct Sim Interval[/b] - Minimum seconds between [code]iplSimulatorRunDirect[/code] on worker wakes when reflection/pathing heavy work is not scheduled for that wake (or after heavy work is skipped). Throttles **direct-path occlusion, transmission, air absorption** independently of [member reflections_sim_interval] / [member pathing_sim_interval].
## [code]0[/code] = run direct every eligible worker wake (default). Try [code]0.02[/code]–[code]0.05[/code] to lower CPU; occlusion may update slightly less often.
@export_range(0.0, 1.0, 0.005) var direct_sim_interval: float:
	get:
		return _direct_sim_interval
	set(v):
		_direct_sim_interval = v
		_on_performance_knob_changed()
var _reflections_sim_interval: float = 0.1
## [b]Reflections Sim Interval[/b] - Minimum seconds between scheduling reflection-heavy simulation ([code]iplSimulatorRunReflections[/code]). [code]0[/code] = every worker tick (highest CPU). [code]0.1[/code] ≈ 100 ms default. Does not throttle direct occlusion/transmission - see [member direct_sim_interval].
@export_range(0.0, 1.0, 0.01) var reflections_sim_interval: float:
	get:
		return _reflections_sim_interval
	set(v):
		_reflections_sim_interval = v
		_on_performance_knob_changed()
var _pathing_sim_interval: float = 0.1
## [b]Pathing Sim Interval[/b] - Minimum seconds between scheduling pathing-heavy simulation ([code]iplSimulatorRunPathing[/code]). Same semantics as [member reflections_sim_interval]; set higher to stagger expensive pathing from reflections.
@export_range(0.0, 1.0, 0.01) var pathing_sim_interval: float:
	get:
		return _pathing_sim_interval
	set(v):
		_pathing_sim_interval = v
		_on_performance_knob_changed()
## Maximum simultaneous sources for realtime reflection simulation (Steam Audio [code]maxNumSources[/code]). Higher values use more CPU and memory.
@export_range(8, 128, 1) var max_simulation_sources: int = 32
## Minimum seconds between worker applications of queued dynamic geometry transforms to Steam Audio (scene commit cost control).
@export_range(0.0, 1.0, 0.005) var dynamic_scene_commit_min_interval: float = 0.0
## When on, players enqueue source updates and ResonanceRuntime applies them once per frame (reduces lock contention).
@export var batch_source_updates: bool = true

# --- Scene Backend & Physics Integration ---
@export_group("Scene Backend & Physics")
var _scene_type: int = 0
## Ray tracer backend. Default = built-in Phonon; Embree = Intel (often faster on CPU).
## Radeon Rays = OpenCL GPU path supported on 64-bit Windows only; other platforms fall back to Default.
## Custom = Steam Audio custom scene: raycasts use Godot 3D physics and simulation runs during the physics frame.
@export_enum("Default:0", "Embree:1", "Radeon Rays:2", "Custom (Godot Physics):3")
var scene_type: int:
	get:
		return _scene_type
	set(v):
		if _scene_type != v:
			_scene_type = v
			notify_property_list_changed()
## Collision mask for Godot [code]PhysicsRayQueryParameters3D[/code] when [member scene_type] is Custom. [code]-1[/code] = all physics layers.
@export_flags_3d_physics var physics_ray_collision_mask: int = -1
## Rays per Phonon job when [member scene_type] is Custom. Values > 1 enable batched Godot ray callbacks.
@export_range(1, 256, 1) var physics_ray_batch_size: int = 16
## OpenCL device type when scene_type is Radeon Rays (or when using TAN). Helps when a GPU has OpenCL issues.
@export_enum("GPU:0", "CPU:1", "Any:2") var opencl_device_type: int = 0
## OpenCL device index (0 = first matching device). Useful when multiple GPUs are present.
@export_range(0, 31, 1) var opencl_device_index: int = 0

# --- Expert ---
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
		_realtime_reflection_max_distance_m = v


func _validate_property(property: Dictionary) -> void:
	if property.name in ["hybrid_reverb_transition_time", "hybrid_reverb_overlap_percent"]:
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
			"pathing_num_vis_samples",
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


## Active custom SOFA for init: [member hrtf_sofa_assets] at [member hrtf_sofa_selected_index], or first list entry with data if that slot is empty. Null if the list is empty or has no valid SOFA data.
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


## Returns realtime_rays unchanged for all platforms. [param os_name] is reserved for future per-OS caps; callers should pass [method OS.get_name].
static func get_effective_realtime_rays(realtime_rays: int, _os_name: String) -> int:
	return realtime_rays


## Derives Godot mix buffer size from Project Settings (audio/driver/output_latency). Matches reverb bus frame_count.
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
	# Closest of 256, 512, 1024, 2048 to match Godot's mix buffer
	var candidates := [256, 512, 1024, 2048]
	var best := 512
	var best_dist := 999999
	for c in candidates:
		var d := abs(raw - c)
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
	direct_binaural = reverb_binaural
	pathing_binaural = reverb_binaural
	_spatial_binaural_config_version = 2
	emit_changed()


## Returns config dictionary for [method ResonanceServer.init_audio_engine] when merged by [method ResonanceRuntime.get_config_dict]. Does not include [member bus] / [member reverb_bus_name]; the runtime node adds [code]context_simd_level[/code] / [code]context_validation[/code] there.
func get_config() -> Dictionary:
	_migrate_spatial_binaural_if_needed()
	var rays := get_effective_realtime_rays(realtime_rays, OS.get_name())
	var mix_rate := int(AudioServer.get_mix_rate())
	var rate := sample_rate_override if sample_rate_override > 0 else mix_rate
	if sample_rate_override > 0 and sample_rate_override != mix_rate:
		push_warning(
			(
				"Nexus Resonance: sample_rate_override (%d) differs from Godot mix rate (%d). No resampling; audio may be affected."
				% [sample_rate_override, mix_rate]
			)
		)
	var frame_size := (
		audio_frame_size if audio_frame_size > 0 else _get_audio_frame_size_from_project()
	)
	return {
		"sample_rate": rate,
		"audio_frame_size": frame_size,
		"audio_frame_size_was_auto": audio_frame_size == 0,
		"ambisonic_order": ambisonic_order,
		"simulation_cpu_cores_percent": simulation_cpu_cores_percent,
		"max_reverb_duration": max_reverb_duration,
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
		"reverb_influence_radius": reverb_influence_radius,
		"reverb_transmission_amount": reverb_transmission_amount,
		"apply_occlusion_to_baked_reflections": apply_occlusion_to_baked_reflections,
		# Native engine flag used for baked-REVERB probe selection (Phase 4). Keep the config key stable even if we
		# later extend reflections_sampling_mode to realtime ray origin.
		"baked_reverb_use_listener_probe": reflections_sampling_mode == 0,
		"pathing_enabled": pathing_enabled,
		"pathing_normalize_eq": pathing_normalize_eq,
		"pathing_num_vis_samples": pathing_num_vis_samples,
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
		"default_reflections_mode": default_reflections_mode,
		"output_direct": true,
		"output_reverb": true
	}


## Creates a default runtime config for editor/fallback when no ResonanceRuntime in scene.
static func create_default() -> ResonanceRuntimeConfig:
	return ResonanceRuntimeConfig.new()

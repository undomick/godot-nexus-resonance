extends RefCounted

## Scenario list for the performance check. Each entry is plain data; perf_scene_builder.gd turns it into nodes.
##
## Keys:
##   id, question: what the scenario answers
##   runtime: ResonanceRuntimeConfig property overrides
##   geometry: "none" | "room" | "dense_room"
##   players: number of ResonancePlayer nodes, playing: whether they play
##   player: ResonancePlayerConfig property overrides
##   source_distance: metres from the listener
##   motion: "none" | "listener" | "sources"
##   dynamic_movers: moving ResonanceDynamicGeometry boxes
##   churn_per_sec: one-shot players spawned per second (freed on finish)
##   stream_toggles_per_sec: main-thread static pack swaps and probe volume add/removes per second
##     (needs bake_probes)
##   listener_reverb: ResonanceListener.apply_reverb, listener_reverb_type: its reverb_type
##   bake_probes: bake a probe volume over the room into RAM before playback (not measured)
##   compare: relative checks against other scenarios, see perf_check.gd

const DIRECT_ONLY := {"reflections_enabled": false, "pathing_enabled_override": 0}
const REALTIME_REFLECTIONS := {
	"reflections_enabled": true, "pathing_enabled_override": 0, "reflections_type": 0
}
const RT_CONVOLUTION := {"realtime_rays": 1024, "reflection_type": 0}
const RT_PARAMETRIC := {"realtime_rays": 1024, "reflection_type": 1}
const SEND_ONLY := {"reflections_enabled": false, "pathing_enabled_override": 0}


static func all() -> Array[Dictionary]:
	return [
		{
			"id": "baseline_empty",
			"question":
			"Cost of a ResonanceRuntime with no geometry and no players (reference for all deltas).",
			"geometry": "none",
			"players": 0,
		},
		{
			"id": "idle_room",
			"question": "Does a static room without sources keep the worker busy?",
			"geometry": "room",
			"players": 0,
			"compare":
			[
				{
					"metric": "worker_cpu_pct",
					"vs": "baseline_empty",
					"max_delta": "idle_worker_delta_pct"
				}
			],
		},
		{
			"id": "stopped_players_16",
			"question": "Do 16 players that are in the tree but not playing cost anything?",
			"geometry": "room",
			"runtime": RT_CONVOLUTION,
			"players": 16,
			"playing": false,
			"player": REALTIME_REFLECTIONS,
			"compare":
			[
				{
					"metric": "worker_cpu_pct",
					"vs": "idle_room",
					"max_delta": "idle_worker_delta_pct"
				},
				{
					"metric": "main_frame_cpu_us",
					"vs": "idle_room",
					"max_delta": "idle_main_delta_us"
				},
			],
		},
		{
			"id": "direct_8",
			"question":
			"Per-voice cost of direct sound (HRTF, occlusion, air absorption) with a moving listener.",
			"geometry": "room",
			"players": 8,
			"player": DIRECT_ONLY,
			"motion": "listener",
		},
		{
			"id": "direct_32",
			"question": "Does direct sound scale linearly up to the 32-source limit?",
			"geometry": "room",
			"players": 32,
			"player": DIRECT_ONLY,
			"motion": "listener",
			"compare":
			[
				{
					"metric": "audio_us_per_voice_block",
					"vs": "direct_8",
					"max_ratio": "voice_scaling_ratio"
				},
				{
					"metric": "main_us_per_player",
					"vs": "direct_8",
					"max_ratio": "voice_scaling_ratio"
				},
			],
		},
		{
			"id": "far_direct_16",
			"question": "Are inaudible sources beyond max_distance cheaper than audible ones?",
			"geometry": "room",
			"players": 16,
			"player": DIRECT_ONLY.merged({"max_distance": 30.0, "attenuation_mode": 1}),
			"source_distance": 80.0,
			"motion": "listener",
			"compare":
			[
				{
					"metric": "audio_us_per_voice_block",
					"vs": "direct_8",
					"max_ratio": "inaudible_voice_cost_ratio"
				}
			],
		},
		{
			"id": "parametric_8",
			"question": "Realtime parametric reverb: worker and audio cost.",
			"geometry": "room",
			"runtime": RT_PARAMETRIC,
			"players": 8,
			"player": REALTIME_REFLECTIONS,
			"motion": "listener",
		},
		{
			"id": "convolution_8",
			"question":
			"Realtime convolution reverb (1st order): worker, per-voice and reverb-bus cost.",
			"geometry": "room",
			"runtime": RT_CONVOLUTION,
			"players": 8,
			"player": REALTIME_REFLECTIONS,
			"motion": "listener",
		},
		{
			"id": "convolution_8_static",
			"question":
			"Same as convolution_8 but nothing moves: is unchanged input simulated again?",
			"geometry": "room",
			"runtime": RT_CONVOLUTION,
			"players": 8,
			"player": REALTIME_REFLECTIONS,
			"motion": "none",
			"compare":
			[
				{
					"metric": "worker_cpu_pct",
					"vs": "convolution_8",
					"max_ratio": "static_scene_worker_ratio"
				}
			],
		},
		{
			"id": "convolution_8_adaptive",
			"question":
			"With a reflections budget far below the real cost, does the adaptive controller lower the ray count?",
			"geometry": "room",
			"runtime": RT_CONVOLUTION.merged({"reflections_adaptive_budget_us": 1000}),
			"players": 8,
			"player": REALTIME_REFLECTIONS,
			"motion": "listener",
			"expect":
			[
				{
					"metric": "rays_min",
					"max": 1023,
					"why": "adaptive budget must reduce rays below 1024"
				}
			],
		},
		{
			"id": "convolution_32",
			"question": "Realtime convolution at the 32-source limit.",
			"geometry": "room",
			"runtime": RT_CONVOLUTION,
			"players": 32,
			"player": REALTIME_REFLECTIONS,
			"motion": "sources",
			"expect":
			[{"metric": "reflections_age_ms_max", "max": 500, "why": "reflections age limit"}],
			"compare":
			[
				{
					"metric": "audio_us_per_voice_block",
					"vs": "convolution_8",
					"max_ratio": "voice_scaling_ratio"
				}
			],
		},
		{
			"id": "listener_reverb_8",
			"question":
			"Realtime convolution reverb at the listener, sources only send: one convolution on the reverb bus.",
			"geometry": "room",
			"runtime": RT_CONVOLUTION,
			"listener_reverb": true,
			"players": 8,
			"player": SEND_ONLY,
			"motion": "listener",
		},
		{
			"id": "listener_reverb_32",
			"question": "Listener reverb at the 32-source limit: does the cost stay flat?",
			"geometry": "room",
			"runtime": RT_CONVOLUTION,
			"listener_reverb": true,
			"players": 32,
			"player": SEND_ONLY,
			"motion": "sources",
			"compare":
			[
				{
					"metric": "audio_us_per_voice_block",
					"vs": "listener_reverb_8",
					"max_ratio": "voice_scaling_ratio"
				}
			],
		},
		{
			"id": "listener_reverb_baked_8",
			"question":
			"Baked listener reverb from probes baked at runtime, sources only send: one convolution on the reverb bus.",
			"geometry": "room",
			"runtime": RT_CONVOLUTION,
			"listener_reverb": true,
			"listener_reverb_type": ResonanceListener.REVERB_BAKED,
			"bake_probes": true,
			"players": 8,
			"player": SEND_ONLY,
			"motion": "listener",
		},
		{
			"id": "dynamic_geometry_10",
			"question":
			"10 moving dynamic meshes with 8 occluded sources: transform queue and scene commits.",
			"geometry": "room",
			"players": 8,
			"player": DIRECT_ONLY,
			"dynamic_movers": 10,
			"motion": "listener",
		},
		{
			"id": "dense_room_8",
			"question": "Room plus ~50k triangles of clutter: does direct simulation stay cheap?",
			"geometry": "dense_room",
			"runtime": RT_CONVOLUTION,
			"players": 8,
			"player": REALTIME_REFLECTIONS,
			"motion": "listener",
		},
		{
			"id": "oneshot_churn",
			"question":
			"Spawning and freeing 10 one-shot players per second: main-thread spikes and dropouts.",
			"geometry": "room",
			"runtime": RT_CONVOLUTION,
			"players": 0,
			"player": REALTIME_REFLECTIONS,
			"churn_per_sec": 10.0,
		},
		{
			"id": "streaming_32",
			"question":
			"Swapping a static pack, adding/removing a probe volume and swapping a geometry material during 32-source reflections: does the main thread stall on the reflections pass?",
			"geometry": "room",
			"runtime": RT_CONVOLUTION,
			"bake_probes": true,
			"players": 32,
			"player": REALTIME_REFLECTIONS,
			"motion": "sources",
			"stream_toggles_per_sec": 4.0,
			"material_swaps_per_sec": 4.0,
			"expect":
			[{"metric": "reflections_age_ms_max", "max": 500, "why": "reflections age limit"}],
		},
		{
			"id": "streaming_direct_32",
			"question":
			"streaming_32 without reflections: main-thread cost of the toggles when no reflections pass holds the lock.",
			"geometry": "room",
			"bake_probes": true,
			"players": 32,
			"player": DIRECT_ONLY,
			"motion": "sources",
			"stream_toggles_per_sec": 4.0,
			"material_swaps_per_sec": 4.0,
		},
	]

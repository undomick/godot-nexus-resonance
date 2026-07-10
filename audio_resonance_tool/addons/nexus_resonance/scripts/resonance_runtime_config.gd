@tool
@icon("res://addons/nexus_resonance/ui/icons/resonance_config.svg")
extends Resource
class_name ResonanceRuntimeConfig

## Recurso de configuración en tiempo de ejecución para Nexus Resonance. Créalo o enlázalo en el nodo ResonanceRuntime (Añadir nodo hijo > ResonanceRuntime). La estructura y el orden de los grupos siguen las opciones de configuración de Steam Audio: HRTF Settings, Ray Tracer Settings, Occlusion Settings, Real-time Reflections, Baked Reverb, Baked Pathing, Simulation Update, Reflection Effect, Hybrid Reverb, Reverb Output, Physics. Emits reflection_type_changed, pathing_enabled_changed y audio_frame_size_changed cuando cambian esos ajustes. Enrutamiento de Godot: [member bus] y [member reverb_bus_name]; el bus del efecto de reverberación envía al mismo destino que [method get_bus_effective].

const Constants = preload("resonance_config_constants.gd")

signal reflection_type_changed(new_type: int)
signal pathing_enabled_changed(enabled: bool)
signal audio_frame_size_changed(new_size: int)

# --- Output & Routing ---
@export_group("Output & Routing")
## Bus de destino para Direct + Pathing (salida del reproductor). Vacío = Master. Respaldo cuando los reproductores utilizan la anulación de bus global (Global).
@export var bus: StringName = &"Master"
## Bus que aloja el [ResonanceAudioEffect] para la señal wet de convolución / TAN y el activador de reverberación. Vacío = ResonanceReverb. El envío de Godot para este bus sigue a [method get_bus_effective] (el mismo que el bus de tiempo de ejecución de Direct + Pathing).
@export var reverb_bus_name: StringName = &"ResonanceReverb"

# --- Audio Engine ---
@export_group("Audio Engine")
## Anulación de la frecuencia de muestreo (sample rate). 0 = usar la frecuencia de mezcla de Godot (sigue la configuración del proyecto). Otros valores = anulación manual; un desajuste puede causar problemas de audio (no realiza remuestreo).
@export_enum(
	"Use Godot Mix Rate:0",
	"22050 Hz:22050",
	"44100 Hz:44100",
	"48000 Hz:48000",
	"96000 Hz:96000",
	"192000 Hz:192000"
)
var sample_rate_override: int = 0
## Tamaño del bloque de procesamiento de Steam Audio (muestras por canal por aplicación del efecto). Debe coincidir con el búfer de mezcla de Godot para un enrutamiento de reverberación estable. Auto (0) = el valor más cercano a 256 / 512 / 1024 / 2048 a partir de la opción [code]audio/driver/output_latency[/code] de la configuración del proyecto y la frecuencia de mezcla. Manual = anulación manual para priorizar latencia frente a uso de CPU; los valores incorrectos corren el riesgo de producir pérdidas de datos (dropouts). Después de cambiarlo mientras el juego se ejecuta desde el editor, [ResonanceRuntime] activa [method ResonanceServer.reinit_audio_engine] a través del señal [signal audio_frame_size_changed].
var _audio_frame_size: int = 0
@export_enum("Auto:0", "256:256", "512:512", "1024:1024", "2048:2048")
## Tamaño del bloque de procesamiento de Steam Audio (muestras por canal por aplicación del efecto). Debe coincidir con el búfer de mezcla de Godot para un enrutamiento de reverberación estable. Auto (0) = el valor más cercano a 256 / 512 / 1024 / 2048 a partir de la opción [code]audio/driver/output_latency[/code] de la configuración del proyecto y la frecuencia de mezcla. Manual = anulación manual para priorizar latencia frente a uso de CPU; los valores incorrectos corren el riesgo de producir pérdidas de datos (dropouts). Después de cambiarlo mientras el juego se ejecuta desde el editor, [ResonanceRuntime] activa [method ResonanceServer.reinit_audio_engine] a través del señal [signal audio_frame_size_changed].
var audio_frame_size: int:
	get:
		return _audio_frame_size
	set(v):
		if _audio_frame_size != v:
			_audio_frame_size = v
			audio_frame_size_changed.emit(v)

# --- HRTF & Spatialization ---
@export_group("Spatialization")
## Utiliza sonido envolvente virtual (virtual surround) en lugar de HRTF.
@export var use_virtual_surround: bool = false
## Ruta directa sin HRTF: recuento de canales de [code]IPLSpeakerLayout[/code] (1 Mono, 2 Estéreo, 4 Quad, 6 5.1, 8 7.1). Con codificación de sonido envolvente y ambisónicos, las fuentes sin HRTF utilizan [code]iplAmbisonicsPanningEffect[/code]; de lo contrario, utilizan [code]iplPanningEffect[/code]. La salida de [code]AudioStreamPlayback[/code] de Godot sigue siendo estéreo (mezcla hacia abajo en el reproductor). Los valores no válidos se convierten en estéreo en tiempo de ejecución. Requiere [method ResonanceServer.reinit_audio_engine] después de cambiarlo en funcionamiento.
@export_enum("Mono:1", "Stereo:2", "Quad:4", "5.1:6", "7.1:8") var direct_speaker_channels: int = 2

@export_subgroup("Headphone HRTF", "")
## Cuando [ResonancePlayerConfig] utiliza direct binaural global, aplica HRTF en la ruta dry / directa (frente al paneo de altavoces). Requiere un HRTF cargado.
@export var direct_binaural: bool = true
## Aplica HRTF al decodificar la salida de la convolución ambisónica / mezclador a estéreo (ruta wet de reflexiones / reverberación).
@export var reverb_binaural: bool = true
## Aplica HRTF en el efecto de trazado de rutas (rutas indirectas). Cuando el HRTF de trazado de rutas de [ResonancePlayerConfig] utiliza el valor global, este valor es el predeterminado.
@export var pathing_binaural: bool = true

@export_storage var _spatial_binaural_config_version: int = 0

@export_subgroup("HRTF", "")
## Ganancia del HRTF en dB para el HRTF predeterminado integrado. Con SOFA personalizado, se añade (en dB) al volumen de cada recurso antes de la conversión de ganancia.
@export_range(-24.0, 24.0, 0.1) var hrtf_volume_db: float = 0.0
## Normalización únicamente para el HRTF predeterminado integrado: None (0) o RMS (1). El recurso SOFA personalizado utiliza [member ResonanceSOFAAsset.norm_type] en el recurso.
@export_enum("None:0", "RMS:1") var hrtf_normalization_type: int = 0
## Recursos SOFA HRTF. [member hrtf_sofa_selected_index] selecciona el archivo activo. Vacío = HRTF integrado predeterminado.
@export var hrtf_sofa_assets: Array[ResonanceSOFAAsset] = []
## Índice en [member hrtf_sofa_assets] para el SOFA activo (limitado en tiempo de ejecución).
@export_range(0, 64, 1) var hrtf_sofa_selected_index: int = 0
## Utiliza interpolación HRTF bilineal.
@export var hrtf_interpolation_bilinear: bool = false

@export_subgroup("Third-person Perspective", "")
## Activa la corrección de perspectiva para la reverberación.
@export var perspective_correction_enabled: bool = false
## Factor de corrección de perspectiva (0.5–2.0).
@export_range(0.5, 2.0, 0.1) var perspective_correction_factor: float = 1.0

# --- Reflections & Reverb ---
@export_group("Reflections & Reverb")
## Se aplica únicamente a las fuentes [ResonancePlayer] con tipo de reflexiones 'Use Global'. Otros reproductores pueden anularlo por fuente. [code]0[/code] = Baked (reverberaciones de sonda): la energía indirecta se almacena por sonda y no es lo mismo que la oclusión de rayos por fuente a través de paredes. [code]1[/code] = Realtime (configura [member realtime_rays] > 0): las reflexiones trazadas por rayos siguen la escena acústica (Embree/Default/Radeon) o la física de Godot (Custom), de modo que las salas selladas y la geometría afectan a las rutas indirectas como en Steam Audio en tiempo real. Cada fuente utiliza una ruta por actualización.
## Algoritmo de reverberación: Convolución (0), Paramétrico (1), Híbrido (2), TrueAudio Next (3, solo GPU AMD).
@export_enum("Baked:0", "Realtime:1") var default_reflections_mode: int = 0
var _reflection_type: int = Constants.REFLECTION_TYPE_CONVOLUTION
## Algoritmo de reverberación: Convolución (0), Paramétrico (1), Híbrido (2), TrueAudio Next (3, solo GPU AMD).
@export_enum("Convolution:0", "Parametric:1", "Hybrid:2", "TrueAudio Next (AMD GPU):3")
var reflection_type: int:
	get:
		return _reflection_type
	set(v):
		if _reflection_type != v:
			_reflection_type = v
			reflection_type_changed.emit(v)
			notify_property_list_changed()
## Orden de ambisónicos para la codificación de reverberación: 1st Order (primer orden), 2nd Order (segundo orden) o 3rd Order (tercer orden). Valores más altos equivalen a mayor detalle espacial, pero mayor uso de CPU.
@export_enum("1st Order:1", "2nd Order:2", "3rd Order:3") var ambisonic_order: int = 1
## Límite superior para la asignación de IR del simulador / ReflectionMixer (segundos): limita [code]simulation_settings.maxDuration[/code] y el tamaño de la IR de convolución empaquetada. No es la longitud de la simulación por cada ejecución (ver [member realtime_simulation_duration]).
## [code]IPLSimulationSharedInputs.numRays[/code] para la simulación de reflexiones en tiempo real. [code]0[/code] = Desactivado (sin rayos en tiempo real). Los preajustes incluyen 8, 16, 32, 64, … hasta 8192; conteos mayores consumen más CPU. Utiliza [member scene_type]; sin características de Embree/OpenCL, la capa nativa puede recurrir al trazador integrado (Default).
@export_range(0.1, 10.0, 0.1) var max_reverb_duration: float = 2.0
var _realtime_rays: int = 0
## [code]IPLSimulationSharedInputs.numRays[/code] para la simulación de reflexiones en tiempo real. [code]0[/code] = Desactivado (sin rayos en tiempo real). Los preajustes incluyen 8, 16, 32, 64, … hasta 8192; conteos mayores consumen más CPU. Utiliza [member scene_type]; sin características de Embree/OpenCL, la capa nativa puede recurrir al trazador integrado (Default).
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
## Distancia mínima para el muestreo de irradiancia (0.05–10.0 m). Solo cuando Realtime Rays > 0.
@export_range(0.05, 10.0, 0.01) var realtime_irradiance_min_distance: float = 0.1
## Muestras difusas por punto de reflexión (8–64).
@export_range(8, 64, 1) var realtime_num_diffuse_samples: int = 32
## Rebotes de reflexión en tiempo real (1–64).
@export_range(1, 64, 1) var realtime_bounces: int = 4
## Longitud de la respuesta al impulso (segundos) para las entradas compartidas de simulación en tiempo real ([code]IPLSimulationSharedInputs.duration[/code]) en cada ejecución. Distinto de [member max_reverb_duration] (límite de IR del asignador / mezclador). Rango de 0.1 a 10.0 s.
@export_range(0.1, 10.0, 0.1) var realtime_simulation_duration: float = 2.0
## Reverberación híbrida: longitud (segundos) de la IR utilizada para convolución antes de la cola paramétrica. Solo cuando reflection_type es Hybrid.
@export_range(0.1, 2.0, 0.1) var hybrid_reverb_transition_time: float = 1.0
## Reverberación híbrida: porcentaje de superposición (0–100) para el desvanecimiento cruzado (crossfade) entre la parte de convolución y la paramétrica.
@export_range(0, 100, 1) var hybrid_reverb_overlap_percent: int = 25

# --- Baked Reverb & Pathing ---
@export_group("Baked Reverb & Pathing")
## Radio de influencia de la reverberación horneada en metros.
@export var reverb_influence_radius: float = 10000.0
## Modo de muestreo de reflexiones (de dónde elige su sonda la REVERBERACIÓN horneada). 0 = Centrado en el oyente (sonda más cercana al oyente; recomendado), 1 = Centrado en la fuente (sonda más cercana a la fuente; comportamiento legado). Nota: actualmente esto solo afecta a la búsqueda de sondas de REVERBERACIÓN horneadas. Las reflexiones en tiempo real de Steam Audio trazan rayos desde el oyente en la API principal, por lo que esta configuración aún no cambia el origen del rayo en tiempo real. Se puede anular por fuente a través de [member ResonancePlayerConfig.reflections_sampling_mode_override].
## Activa el trazado de rutas (propagación de sonido por múltiples rutas). Requiere trazado de rutas horneado en los volúmenes de sondas (Probe Volumes).
@export_enum("Listener-centric:0", "Source-centric:1") var reflections_sampling_mode: int = 0
var _pathing_enabled: bool = false
## Activa el trazado de rutas (propagación de sonido por múltiples rutas). Requiere trazado de rutas horneado en los volúmenes de sondas (Probe Volumes).
@export var pathing_enabled: bool:
	get:
		return _pathing_enabled
	set(v):
		if _pathing_enabled != v:
			_pathing_enabled = v
			pathing_enabled_changed.emit(v)
			notify_property_list_changed()
## Trazado de rutas: normaliza la ecualización (EQ) en la salida del efecto de ruta. Evita que el trazado de rutas suene demasiado brillante.
@export var pathing_normalize_eq: bool = true
## Trazado de rutas en tiempo real: numVisSamples de Steam Audio (1–16). Menor = menos CPU; la calidad del horneado utiliza bake_pathing_num_samples en ResonanceBakeConfig.
@export_range(1, 16, 1) var pathing_num_vis_samples: int = 4
## Validación de rutas predeterminada cuando una ResonancePlayerConfig utiliza 'Use Global' (-1) para [member ResonancePlayerConfig.path_validation_override]. Valida las rutas horneadas frente a la geometría dinámica en cada actualización.
@export var path_validation_enabled: bool = true
## Búsqueda de rutas alternativas predeterminada cuando un reproductor utiliza 'Use Global' (-1) para [member ResonancePlayerConfig.find_alternate_paths_override]. Solo se aplica cuando la validación de rutas está efectivamente activada para esa fuente.
@export var find_alternate_paths: bool = false
## Cuánto amortigua la transmisión a la reverberación (0–1). 0 = sin amortiguación, 1 = amortiguación total (reverberación escalada por la transmisión del material). Solo se consulta para las rutas de REVERBERACIÓN horneada cuando la amortiguación de oclusión de la ruta wet está activada (ver [member apply_occlusion_to_baked_reflections] y [member ResonancePlayerConfig.apply_occlusion_to_baked_reflections_override]). Las reflexiones en tiempo real y STATICSOURCE/STATICLISTENER ya codifican la oclusión de fuente→oyente en la IR. Se puede establecer un valor por fuente a través de [member ResonancePlayerConfig.reverb_transmission_amount_input].
@export_range(0.0, 1.0, 0.01) var reverb_transmission_amount: float = 1.0
## Cuando está activado, las reflexiones de REVERBERACIÓN horneada también escalan su entrada wet según el factor de oclusión/transmisión de la ruta directa. Por defecto está desactivado: la oclusión de línea directa no puede distinguir una 'fuente a la vuelta de la esquina en la misma habitación abierta' (la habitación del oyente sigue excitada a través de aberturas/difracción; la convolución en tiempo real sigue sonando fuerte) de una 'fuente verdaderamente aislada'. Activar esto amortigua ambos casos de manera uniforme, lo que hace que la REVERBERACIÓN horneada suene menos plausible que la de tiempo real para el caso común de 'a la vuelta de la esquina'. Prefiere [code]reflections_type = Realtime[/code] por fuente o el flujo de trabajo de horneado STATICSOURCE para reflexiones precisas de exteriores a interiores. Se puede anular por fuente a través de [member ResonancePlayerConfig.apply_occlusion_to_baked_reflections_override] (por ejemplo, activándolo solo para fuentes de truenos o lluvia en exteriores mientras que las fuentes de interiores conservan alimentaciones wet plausibles). No tiene efecto en las rutas de tiempo real, STATICSOURCE o STATICLISTENER.
@export var apply_occlusion_to_baked_reflections: bool = false
# --- Occlusion & Transmission ---
@export_group("Occlusion & Transmission")
## Oclusión: Raycast (0) = prueba de impacto binaria; Volumetric (1) = oclusión fraccional a partir de muestras ([code]numOcclusionSamples[/code]). Independiente de [member transmission_type].
@export_enum("Raycast:0", "Volumetric:1") var occlusion_type: int = 1
## Límite del simulador para oclusión volumétrica ([code]maxNumOcclusionSamples[/code]). Las muestras de oclusión por fuente se limitan a este valor.
@export_range(1, 128, 1) var max_occlusion_samples: int = 64
## Modo de frecuencia de transmisión del efecto directo ([code]IPLTransmissionType[/code] de Steam Audio): FreqIndependent (0) = un coeficiente mezclado; FreqDependent (1) = bandas baja/media/alta. Controla cómo se aplica la transmisión de material en el procesador directo, no la mezcla en los bordes de la geometría. [code]IPLSimulationInputs[/code] de Steam Audio no tiene un interruptor de 'transmisión volumétrica' separado (solo [code]numTransmissionRays[/code] para la profundidad de la ruta).
@export_enum("FreqIndependent:0", "FreqDependent:1") var transmission_type: int = 1
## Valor global predeterminado para [code]numTransmissionRays[/code] de Steam Audio (superficies máximas a lo largo de la ruta de transmisión, 1–256): estado inicial de la fuente del simulador y cuando [member ResonancePlayerConfig.max_transmission_surfaces_override] es **Use Global** ([code]0[/code]), o como respaldo de C++ al leer [member ResonancePlayerConfig.max_transmission_surfaces] por fuente.
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

## Fracción de núcleos de CPU para hilos de simulación de Steam Audio (0–1). Por defecto 0.15; valores mayores equivalen a mayor paralelismo para reflexiones/trazado de rutas, pero mayor uso de CPU.
## [b]Intervalo de Sim Directo[/b] — Segundos mínimos entre ejecuciones de [code]iplSimulatorRunDirect[/code] en ciclos de trabajo que no programen ya pases pesados de reflexiones o de trazado de rutas (o cuando estos se omitan). Utilízalo para limitar las actualizaciones de **oclusión de ruta directa/transmisión/absorción de aire** independientemente de [member reflections_sim_interval] y [member pathing_sim_interval].
## [b]0[/b] (por defecto): ejecuta la simulación directa en cada activación del hilo de trabajo donde este se ejecute (misma capacidad de respuesta que antes de que existiera este control).
## Valores pequeños (por ejemplo, [b]0.02[/b]–[b]0.05[/b]): menos actualizaciones directas cuando las reflexiones/trazado de rutas ya son escasos (reduce el uso de CPU; la oclusión puede reaccionar un poco más lento hasta el próximo ciclo elegible).
## Cuando un ciclo programa reflexiones o trazado de rutas, la simulación directa sigue la programación combinada del hilo de trabajo para esa activación.
@export_range(0.0, 1.0, 0.01) var simulation_cpu_cores_percent: float = 0.15
var _direct_sim_interval: float = 0.0
## [b]Intervalo de Sim Directo[/b] — Segundos mínimos entre ejecuciones de [code]iplSimulatorRunDirect[/code] en ciclos de trabajo que no programen ya pases pesados de reflexiones o de trazado de rutas (o cuando estos se omitan). Utilízalo para limitar las actualizaciones de **oclusión de ruta directa/transmisión/absorción de aire** independientemente de [member reflections_sim_interval] y [member pathing_sim_interval].
## [b]0[/b] (por defecto): ejecuta la simulación directa en cada activación del hilo de trabajo donde este se ejecute (misma capacidad de respuesta que antes de que existiera este control).
## Valores pequeños (por ejemplo, [b]0.02[/b]–[b]0.05[/b]): menos actualizaciones directas cuando las reflexiones/trazado de rutas ya son escasos (reduce el uso de CPU; la oclusión puede reaccionar un poco más lento hasta el próximo ciclo elegible).
## Cuando un ciclo programa reflexiones o trazado de rutas, la simulación directa sigue la programación combinada del hilo de trabajo para esa activación.
@export_range(0.0, 1.0, 0.005) var direct_sim_interval: float:
	get:
		return _direct_sim_interval
	set(v):
		_direct_sim_interval = v
		_on_performance_knob_changed()
## [b]Intervalo de Sim de Reflexiones[/b] — Segundos mínimos entre programaciones de simulaciones pesadas en reflexiones ([code]iplSimulatorRunReflections[/code]). [b]0[/b]: programar en cada ciclo de trabajo (mayor CPU). [b]0.1[/b]: como máximo una ola de programación de reflexiones cada ~100 ms (buen valor predeterminado). Valores más altos reducen el uso de CPU; las actualizaciones de IR en tiempo real pueden retrasarse ligeramente. Independiente de [member pathing_sim_interval] y [member direct_sim_interval].
var _reflections_sim_interval: float = 0.1
## [b]Intervalo de Sim de Reflexiones[/b] — Segundos mínimos entre programaciones de simulaciones pesadas en reflexiones ([code]iplSimulatorRunReflections[/code]). [b]0[/b]: programar en cada ciclo de trabajo (mayor CPU). [b]0.1[/b]: como máximo una ola de programación de reflexiones cada ~100 ms (buen valor predeterminado). Valores más altos reducen el uso de CPU; las actualizaciones de IR en tiempo real pueden retrasarse ligeramente. Independiente de [member pathing_sim_interval] y [member direct_sim_interval].
@export_range(0.0, 1.0, 0.01) var reflections_sim_interval: float:
	get:
		return _reflections_sim_interval
	set(v):
		_reflections_sim_interval = v
		_on_performance_knob_changed()
## [b]Intervalo de Sim de Trazado de Rutas[/b] — Segundos mínimos entre programaciones de simulaciones pesadas en trazado de rutas ([code]iplSimulatorRunPathing[/code]). Misma semántica que [member reflections_sim_interval]; increméntalo para espaciar el costoso trazado de rutas de las reflexiones (por ejemplo, validación de rutas / rutas alternativas).
var _pathing_sim_interval: float = 0.1
## [b]Intervalo de Sim de Trazado de Rutas[/b] — Segundos mínimos entre programaciones de simulaciones pesadas en trazado de rutas ([code]iplSimulatorRunPathing[/code]). Misma semántica que [member reflections_sim_interval]; increméntalo para espaciar el costoso trazado de rutas de las reflexiones (por ejemplo, validación de rutas / rutas alternativas).
@export_range(0.0, 1.0, 0.01) var pathing_sim_interval: float:
	get:
		return _pathing_sim_interval
	set(v):
		_pathing_sim_interval = v
		_on_performance_knob_changed()
## Número máximo de fuentes simultáneas para la simulación de reflexiones en tiempo real ([code]maxNumSources[/code]) y TrueAudio Next [code]maxSources[/code].
@export_range(8, 128, 1) var max_simulation_sources: int = 32
## Segundos mínimos entre aplicaciones del hilo de trabajo de transformaciones de mallas instanciadas dinámicas en cola a Steam Audio (un lote de [code]iplSceneCommit[/code] por aplicación). [code]0[/code] = aplicar en cada ciclo de simulación que tenga actualizaciones en cola. Un valor [code]> 0[/code] puede reducir el costo de [code]rtcCommitScene[/code] en escenas Embree grandes a expensas de una oclusión desactualizada durante el movimiento. Emparéjalo con [method ResonanceGeometry.flush_dynamic_acoustic_transform] cuando el movimiento se detenga.
@export_range(0.0, 1.0, 0.005) var dynamic_scene_commit_min_interval: float = 0.0
## When on, players enqueue source updates and ResonanceRuntime applies them once per frame (reduces lock contention).
@export var batch_source_updates: bool = true

# --- Scene Backend & Physics Integration ---
## Backend del trazador de rayos: Default (0) = Phonon integrado; Embree (1) = CPU Intel; Radeon Rays (2) = OpenCL (orientado a Windows de 64 bits); Custom (3) = [code]IPL_SCENETYPE_CUSTOM[/code] utilizando el método [code]intersect_ray[/code] de la física 3D de Godot ([member physics_ray_collision_mask]). Custom ejecuta la simulación de Steam Audio en el hilo principal; las mallas de [ResonanceGeometry] no se cargan. Los horneados y recursos de malla siguen utilizando el trazador integrado. Evita [code]physics/3d/run_on_separate_thread[/code] si es posible (consulta las notas sobre hilos de física de Godot).
@export_group("Scene Backend & Physics")
var _scene_type: int = 0
## Backend del trazador de rayos: Default (0) = Phonon integrado; Embree (1) = CPU Intel; Radeon Rays (2) = OpenCL (orientado a Windows de 64 bits); Custom (3) = [code]IPL_SCENETYPE_CUSTOM[/code] utilizando el método [code]intersect_ray[/code] de la física 3D de Godot ([member physics_ray_collision_mask]). Custom ejecuta la simulación de Steam Audio en el hilo principal; las mallas de [ResonanceGeometry] no se cargan. Los horneados y recursos de malla siguen utilizando el trazador integrado. Evita [code]physics/3d/run_on_separate_thread[/code] si es posible (consulta las notas sobre hilos de física de Godot).
@export_enum("Default:0", "Embree:1", "Radeon Rays:2", "Custom (Godot Physics):3")
var scene_type: int:
	get:
		return _scene_type
	set(v):
		if _scene_type != v:
			_scene_type = v
			notify_property_list_changed()
## Máscara de colisión de rayos de física cuando [member scene_type] es Custom. [code]-1[/code] significa todas las capas. Misma semántica que [code]PhysicsRayQueryParameters3D.collision_mask[/code].
@export_flags_3d_physics var physics_ray_collision_mask: int = -1
## Cuando [member scene_type] es Custom: [code]IPLSimulationSettings.rayBatchSize[/code] y devoluciones de llamadas (callbacks) de traza de Godot agrupadas en lote cuando es [code]> 1[/code] (Phonon BatchedReflectionSimulator). Limitado a 1–256 en la capa nativa. Ignorado para Default/Embree/Radeon (Nexus pasa un tamaño de lote de 1).
@export_range(1, 256, 1) var physics_ray_batch_size: int = 16
## Tipo de dispositivo OpenCL cuando scene_type es Radeon Rays o reflection_type es TrueAudio Next: GPU (0), CPU (1) o Any (2). Ayuda cuando la GPU presenta problemas con OpenCL.
@export_enum("GPU:0", "CPU:1", "Any:2") var opencl_device_type: int = 0
## Índice del dispositivo OpenCL (0 = primer dispositivo coincidente). Utilízalo cuando haya varias GPU presentes.
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
	return bus if not bus.is_empty() else &"Master"


## Returns effective reverb bus name. Empty config = ResonanceReverb.
func get_reverb_bus_name_effective() -> StringName:
	return reverb_bus_name if not reverb_bus_name.is_empty() else &"ResonanceReverb"


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

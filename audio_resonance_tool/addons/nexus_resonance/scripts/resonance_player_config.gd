@tool
@icon("res://addons/nexus_resonance/ui/icons/resonance_config.svg")
extends Resource
class_name ResonancePlayerConfig

## Configuración por fuente para ResonancePlayer. Asígnalo desde ResonancePlayer como player_config para preajustes reutilizables. Guárdalo como .tres para compartirlo entre escenas. Recurre a las variables miembro del nodo cuando es nulo. pathing_probe_volume permanece en el nodo reproductor (específico de la escena).

# --- Distance / Attenuation ---
@export_group("Distance")
## Distancia (metros) a la cual el sonido está al volumen máximo en la ruta de reproducción directa y en el [code]distanceAttenuationModel[/code] de Phonon cuando la atenuación de distancia de simulación está activa.
@export_range(0.1, 100.0, 0.1) var min_distance: float = 1.0
## Distancia máxima (metros) para la atenuación directa de la reproducción y para el modelo de distancia de la simulación (fin de atenuación lineal o por curva; corte inverso en la corrección de respuestas al impulso (IR)).
## Inverse (0) = atenuación de distancia inversa de Steam en reproducción directa; la simulación puede aplicar [code]distanceAttenuationModel[/code] a las reflexiones (corrección de IR) cuando está activado. Linear (1) y Curve (2) comparten la atenuación en la directa y en la simulación. Disabled (3) = sin atenuación por distancia en la reproducción directa; modelo de distancia de simulación desactivado. El comportamiento legado [code]distance_attenuation_simulation_enabled = false[/code] con el modo 0 se trata como Disabled (3). La señal procesada (wet) de las reflexiones en reproducción no se multiplica por esta curva (paridad con Unity Spatialize); utiliza únicamente [member reflections_mix_level] y la oclusión wet horneada.
@export_range(1.0, 2000.0, 1.0) var max_distance: float = 500.0
var _attenuation_mode: int = 0
## Inverse (0) = atenuación de distancia inversa de Steam en reproducción directa; la simulación puede aplicar [code]distanceAttenuationModel[/code] a las reflexiones (corrección de IR) cuando está activado. Linear (1) y Curve (2) comparten la atenuación en la directa y en la simulación. Disabled (3) = sin atenuación por distancia en la reproducción directa; modelo de distancia de simulación desactivado. El comportamiento legado [code]distance_attenuation_simulation_enabled = false[/code] con el modo 0 se trata como Disabled (3). La señal procesada (wet) de las reflexiones en reproducción no se multiplica por esta curva (paridad con Unity Spatialize); utiliza únicamente [member reflections_mix_level] y la oclusión wet horneada.
@export_enum("Inverse:0", "Linear:1", "Curve:2", "Disabled:3") var attenuation_mode: int:
	get:
		return _attenuation_mode
	set(v):
		if _attenuation_mode != v:
			_attenuation_mode = v
			notify_property_list_changed()
## Curva de atenuación personalizada. Se utiliza cuando attenuation_mode es Curve.
@export var attenuation_curve: Curve = null

# --- Direct Sound ---
@export_group("Direct Sound")
## Radio de la fuente de sonido en metros.
## Activa la absorción del aire basada en la distancia.
@export_range(0.1, 10.0, 0.1) var source_radius: float = 1.0
var _air_absorption_enabled: bool = true
## Activa la absorción del aire basada en la distancia.
@export var air_absorption_enabled: bool = true:
	get:
		return _air_absorption_enabled
	set(v):
		if _air_absorption_enabled != v:
			_air_absorption_enabled = v
			notify_property_list_changed()
## Origen de la absorción del aire: Simulation (0) = basado en física, User Defined (1) = utiliza los deslizadores de baja/media/alta.
var _air_absorption_input: int = 0
## Origen de la absorción del aire: Simulation (0) = basado en física, User Defined (1) = utiliza los deslizadores de baja/media/alta.
@export_enum("Simulation Defined:0", "User Defined:1") var air_absorption_input: int:
	get:
		return _air_absorption_input
	set(v):
		if _air_absorption_input != v:
			_air_absorption_input = v
			notify_property_list_changed()
## Ecualización de banda baja (≤800 Hz). 0 = completamente atenuado, 1 = sin cambios. Solo cuando air_absorption_input es User Defined.
@export_range(0.0, 1.0, 0.01) var air_absorption_low: float = 1.0
## Ecualización de banda media (800 Hz–8 kHz).
@export_range(0.0, 1.0, 0.01) var air_absorption_mid: float = 1.0
## Ecualización de banda alta (≥8 kHz).
@export_range(0.0, 1.0, 0.01) var air_absorption_high: float = 1.0

# --- Directivity ---
## Activa la fuente de sonido direccional.
@export_group("Directivity")
var _directivity_enabled: bool = false
## Activa la fuente de sonido direccional.
@export var directivity_enabled: bool:
	get:
		return _directivity_enabled
	set(v):
		if _directivity_enabled != v:
			_directivity_enabled = v
			notify_property_list_changed()
## Origen de la directividad: 0 = Simulation (dipolo), 1 = User Defined (utiliza directivity_value).
var _directivity_input: int = 0
## Origen de la directividad: 0 = Simulation (dipolo), 1 = User Defined (utiliza directivity_value).
@export_enum("Simulation Defined:0", "User Defined:1") var directivity_input: int:
	get:
		return _directivity_input
	set(v):
		if _directivity_input != v:
			_directivity_input = v
			notify_property_list_changed()
## Shape: 0 = Omnidirectional, 1 = Dipole.
@export_range(0.0, 1.0, 0.01) var directivity_weight: float = 0.0
## Nitidez del patrón de directividad (0-4). Solo cuando directivity_input es Simulation.
@export_range(0.0, 4.0, 0.1) var directivity_power: float = 1.0
## Atenuación de directividad (0-1). Solo cuando directivity_input es User Defined.
@export_range(0.0, 1.0, 0.01) var directivity_value: float = 1.0

# --- Output ---
## Bus para Direct + Pathing. -1 = Usar Global (RuntimeConfig.bus), 0 = Personalizado (utiliza bus_name).
@export_group("Output")
var _bus_override: int = -1
## Bus para Direct + Pathing. -1 = Usar Global (RuntimeConfig.bus), 0 = Personalizado (utiliza bus_name).
@export_enum("Use Global:-1", "Custom:0") var bus_override: int = -1:
	get:
		return _bus_override
	set(v):
		if _bus_override != v:
			_bus_override = v
			notify_property_list_changed()
## Bus para Direct + Pathing cuando bus_override is Custom. Must exist in Audio Bus Layout.
## Anulación de bus de reverberación. -1 = Usar Global (RuntimeConfig.reverb_bus_name), 0 = Personalizado (utiliza reverb_bus_name). Para Paramétrico/Híbrido, esto selecciona el bus de salida wet de división cuando difiere del bus dry. Para Convolución/TAN, el mezclador de reflexiones de Steam sigue siendo global; este campo no crea un mezclador de convolución adicional por fuente.
@export var bus_name: StringName = &"Master"
var _reverb_bus_override: int = -1
## Anulación de bus de reverberación. -1 = Usar Global (RuntimeConfig.reverb_bus_name), 0 = Personalizado (utiliza reverb_bus_name). Para Paramétrico/Híbrido, esto selecciona el bus de salida wet de división cuando difiere del bus dry. Para Convolución/TAN, el mezclador de reflexiones de Steam sigue siendo global; este campo no crea un mezclador de convolución adicional por fuente.
@export_enum("Use Global:-1", "Custom:0") var reverb_bus_override: int = -1:
	get:
		return _reverb_bus_override
	set(v):
		if _reverb_bus_override != v:
			_reverb_bus_override = v
			notify_property_list_changed()

## Bus para la señal procesada (wet) dividida de Paramétrico/Híbrido cuando reverb_bus_override es Custom (debe existir en el diseño de buses de audio). Cuando los buses dry y wet difieren, wet se reproduce en este bus. El wet de Convolución/TAN permanece en el bus [member ResonanceRuntimeConfig.reverb_bus_name] del tiempo de ejecución; su envío de Godot sigue a [member ResonanceRuntimeConfig.bus].
@export var reverb_bus_name: StringName = &"ResonanceReverb"

# --- Performance ---
@export_group("Performance")
## Segundos mínimos entre actualizaciones completas de parámetros de reproducción. 0 = cada fotograma (sujeto a otros filtros de paso).
@export_range(0.0, 0.5, 0.005) var playback_parameter_min_interval: float = 0.0
## Movimiento mínimo de la fuente (metros) para activar una actualización completa cuando se combina con playback_parameter_min_interval.
@export_range(0.0, 50.0, 0.05) var playback_parameter_min_move: float = 0.0
## Constante de tiempo de suavizado exponencial (segundos) para la oclusión y transmisión derivadas de la simulación. 0 = desactivado. Cuando es mayor que 0, los parámetros de reproducción se envían en cada fotograma mientras se aplica el suavizado. Solo para oclusión/transmisión definidas por simulación (Simulation Defined).
@export_range(0.0, 0.5, 0.005) var playback_coeff_smoothing_time: float = 0.0

# --- Occlusion ---
@export_group("Occlusion")
## Cuando es [code]false[/code], no se simulan los rayos de oclusión; utiliza [member occlusion_input] definido por el usuario y [member occlusion_value] para oclusión manual.
## Origen de la oclusión: 0 = Simulation (física), 1 = User Defined (utiliza occlusion_value).
@export var simulation_occlusion_enabled: bool = true
var _occlusion_input: int = 0
## Origen de la oclusión: 0 = Simulation (física), 1 = User Defined (utiliza occlusion_value).
@export_enum("Simulation Defined:0", "User Defined:1") var occlusion_input: int:
	get:
		return _occlusion_input
	set(v):
		if _occlusion_input != v:
			_occlusion_input = v
			notify_property_list_changed()
## Atenuación de oclusión (0-1). Solo cuando occlusion_input es User Defined.
## [code]2[/code] = Usar Global ([member ResonanceRuntimeConfig.occlusion_type]). [code]0[/code] = Raycast. [code]1[/code] = Volumétrico ([member occlusion_samples]). Se aplica cuando se utiliza la oclusión por simulación. Los valores no negativos mantienen fiable la enumeración del inspector de Godot; los recursos más antiguos pueden almacenar [code]-1[/code] para usar global (normalizado al cargar).
@export_range(0.0, 1.0, 0.01) var occlusion_value: float = 1.0
var _occlusion_type_override: int = 2
## [code]2[/code] = Usar Global ([member ResonanceRuntimeConfig.occlusion_type]). [code]0[/code] = Raycast. [code]1[/code] = Volumétrico ([member occlusion_samples]). Se aplica cuando se utiliza la oclusión por simulación. Los valores no negativos mantienen fiable la enumeración del inspector de Godot; los recursos más antiguos pueden almacenar [code]-1[/code] para usar global (normalizado al cargar).
@export_enum("Use Global:2", "Raycast:0", "Volumetric:1") var occlusion_type_override: int = 2:
	get:
		return _occlusion_type_override
	set(v):
		var nv := v
		if nv == -1:
			nv = 2
		if nv != 0 and nv != 1 and nv != 2:
			nv = 2
		if _occlusion_type_override != nv:
			_occlusion_type_override = nv
			notify_property_list_changed()
## Rayos por fuente para oclusión volumétrica (1–64, [code]numOcclusionSamples[/code] de Steam Audio). El deslizador del inspector es editable solo cuando [member occlusion_type_override] es **Volumetric** ([code]1[/code]); de lo contrario, utiliza el tipo de oclusión global/tiempo de ejecución sin cantidad de muestras por fuente en la interfaz de usuario. Valores mayores equivalen a una fracción de oclusión más estable cerca de los límites.
@export_range(1, 64, 1) var occlusion_samples: int = 64

# --- Transmission ---
@export_group("Transmission")
## Cuando es [code]false[/code], no se simula la transmisión a través de la geometría; utiliza [member transmission_input] definido por el usuario para bandas manuales.
## Origen de la transmisión: 0 = Simulation, 1 = User Defined (utiliza transmisión baja/media/alta).
@export var simulation_transmission_enabled: bool = true
var _transmission_input: int = 0
## Origen de la transmisión: 0 = Simulation, 1 = User Defined (utiliza transmisión baja/media/alta).
@export_enum("Simulation Defined:0", "User Defined:1") var transmission_input: int:
	get:
		return _transmission_input
	set(v):
		if _transmission_input != v:
			_transmission_input = v
			notify_property_list_changed()
## Transmisión de banda baja (0-1). Solo cuando transmission_input es User Defined.
@export_range(0.0, 1.0, 0.01) var transmission_low: float = 1.0
## Transmisión de banda media (0-1). Solo cuando transmission_input es User Defined.
@export_range(0.0, 1.0, 0.01) var transmission_mid: float = 1.0
## Transmisión de banda alta (0-1). Solo cuando transmission_input es User Defined.
@export_range(0.0, 1.0, 0.01) var transmission_high: float = 1.0
## -1 = usar tipo de transmisión del tiempo de ejecución. 0 = independiente de la frecuencia. 1 = dependiente de la frecuencia (3 bandas), solo para el efecto directo.
@export_enum("Use Global:-1", "Frequency Independent:0", "Frequency Dependent:1")
var transmission_type_override: int = -1
## [code]0[/code] = Usar Global ([member ResonanceRuntimeConfig.max_transmission_surfaces]), por defecto. [code]1[/code] = User Defined: utiliza [member max_transmission_surfaces]. Los recursos antiguos pueden tener [code]-1[/code] para usar global; el reproductor nativo lo normaliza a [code]0[/code].
var _max_transmission_surfaces_override: int = 0
## [code]0[/code] = Usar Global ([member ResonanceRuntimeConfig.max_transmission_surfaces]), por defecto. [code]1[/code] = User Defined: utiliza [member max_transmission_surfaces]. Los recursos antiguos pueden tener [code]-1[/code] para usar global; el reproductor nativo lo normaliza a [code]0[/code].
@export_enum("Use Global:0", "User Defined:1") var max_transmission_surfaces_override: int = 0:
	get:
		return _max_transmission_surfaces_override
	set(v):
		var nv := v
		if nv == -1:
			nv = 0
		if nv != 0 and nv != 1:
			nv = 0
		if _max_transmission_surfaces_override != nv:
			_max_transmission_surfaces_override = nv
			notify_property_list_changed()
## Superficies máximas a lo largo de la ruta de transmisión desde el oyente (1–256; [code]numTransmissionRays[/code] de Steam Audio). El deslizador del inspector es editable solo cuando [member max_transmission_surfaces_override] es User Defined ([code]1[/code]). Limita la profundidad de superficies apiladas en la ruta, no la mezcla lateral entre materiales en un borde.
@export_range(1, 256, 1) var max_transmission_surfaces: int = 16

# --- Reflections (per-source) ---
## Simulación de reflexiones: -1 = Usar Global (reverberación horneada), 0 = Tiempo real, 1 = Reverberación horneada, 2 = Fuente estática horneada (Baked Static Source), 3 = Oyente estático horneado (Baked Static Listener).
@export_group("Reflections")
var _reflections_type: int = -1
## Simulación de reflexiones: -1 = Usar Global (reverberación horneada), 0 = Tiempo real, 1 = Reverberación horneada, 2 = Fuente estática horneada (Baked Static Source), 3 = Oyente estático horneado (Baked Static Listener).
@export_enum(
	"Use Global:-1",
	"Realtime:0",
	"Baked Reverb:1",
	"Baked Static Source:2",
	"Baked Static Listener:3"
)
var reflections_type: int = -1:
	get:
		return _reflections_type
	set(v):
		if _reflections_type != v:
			_reflections_type = v
			notify_property_list_changed()
## Cuando reflections_type es Baked Static Source: nodo cuya posición fue horneada. Vacío = usar la posición del reproductor.
@export var current_baked_source: NodePath = NodePath()
## Cuando reflections_type es Baked Static Listener: nodo (por ejemplo, el oyente) cuya posición fue horneada. Vacío = usar el oyente activo.
@export var current_baked_listener: NodePath = NodePath()
## Activa las reflexiones para esta fuente. -1 = Usar Global, 0 = Desactivado, 1 = Activado.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var reflections_enabled: int = -1
## Activa el trazado de rutas para esta fuente. -1 = Usar Global, 0 = Desactivado, 1 = Activado.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var pathing_enabled_override: int = -1
## Anulación por fuente de [member ResonanceRuntimeConfig.apply_occlusion_to_baked_reflections]. -1 = Usar Global, 0 = Desactivado, 1 = Activado. Activado = amortigua la entrada wet de REVERBERACIÓN horneada de esta fuente según la oclusión/transmisión de su ruta directa. Desactivado = mantiene la entrada wet sin atenuar independientemente de la bandera global. Consulta [code]docs/baked-reflections-and-outdoor-sources.md[/code] para guía sobre el flujo de trabajo.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1")
var apply_occlusion_to_baked_reflections_override: int = -1
## Anulación por fuente para [member ResonanceRuntimeConfig.reflections_sampling_mode]. -1 = Usar Global, 0 = Centrado en el oyente (selecciona la sonda de REVERBERACIÓN horneada más cercana al oyente), 1 = Centrado en la fuente (selecciona la sonda más cercana a la fuente; comportamiento legado). Nota: actualmente esto solo afecta a la búsqueda de sondas de REVERBERACIÓN horneadas; no cambia el origen del rayo de reflexión en tiempo real.
@export_enum("Use Global:-1", "Listener-centric:0", "Source-centric:1")
var reflections_sampling_mode_override: int = -1
## 0 = Usar Global ([member ResonanceRuntimeConfig.reverb_transmission_amount]), 1 = User Defined (utiliza [member reverb_transmission_amount]). Solo efectivo cuando la amortiguación de oclusión de la ruta wet está activada (global o [member apply_occlusion_to_baked_reflections_override]).
var _reverb_transmission_amount_input: int = 0
## 0 = Usar Global ([member ResonanceRuntimeConfig.reverb_transmission_amount]), 1 = User Defined (utiliza [member reverb_transmission_amount]). Solo efectivo cuando la amortiguación de oclusión de la ruta wet está activada (global o [member apply_occlusion_to_baked_reflections_override]).
@export_enum("Use Global:0", "User Defined:1") var reverb_transmission_amount_input: int = 0:
	get:
		return _reverb_transmission_amount_input
	set(v):
		if _reverb_transmission_amount_input != v:
			_reverb_transmission_amount_input = v
			notify_property_list_changed()
## Amortiguación de transmisión por fuente en la reverberación (0-1). 0 = sin amortiguación, 1 = amortiguación total. Solo se utiliza cuando [member reverb_transmission_amount_input] es User Defined y la amortiguación de oclusión de la ruta wet está activada.
@export_range(0.0, 1.0, 0.01) var reverb_transmission_amount: float = 1.0

# --- Pathing ---
@export_group("Pathing")
## Validación de rutas (oclusión dinámica de rutas horneadas). [code]-1[/code] = Usar Global ([member ResonanceRuntimeConfig.path_validation_enabled]). [code]0[/code] = Desactivado. [code]1[/code] = Activado. Los costos se escalan con [member ResonanceRuntimeConfig.pathing_num_vis_samples] y el trazador de rayos; ajusta [member ResonanceRuntimeConfig.pathing_sim_interval] (y [member ResonanceRuntimeConfig.reflections_sim_interval]) y utiliza [method ResonanceServer.get_simulation_worker_timing] para [code]us_run_pathing[/code].
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var path_validation_override: int = -1
## Buscar rutas alternativas cuando una ruta horneada está ocluida. [code]-1[/code] = Usar Global ([member ResonanceRuntimeConfig.find_alternate_paths]). [code]0[/code] = Desactivado. [code]1[/code] = Activado. Solo es efectivo cuando la validación de rutas está activa para esta fuente. Consume mucha CPU; ajusta [member ResonanceRuntimeConfig.pathing_sim_interval] y [member ResonanceRuntimeConfig.pathing_num_vis_samples] si se activa.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var find_alternate_paths_override: int = -1

# --- Mix Levels ---
@export_group("Mix Levels")
## Escala conjuntamente [member direct_mix_level], [member reflections_mix_level] y [member pathing_mix_level] juntos. Utilízalo cuando el flujo no tiene un control de volumen fiable (por ejemplo, [AudioStreamSynchronized]).
@export_range(0.0, 10.0, 0.01) var master_mix_level: float = 1.0
## Volumen de la ruta de sonido directo (0-10).
@export_range(0.0, 10.0, 0.01) var direct_mix_level: float = 1.0
## Volumen de las reflexiones y reverberación (0-10).
@export_range(0.0, 10.0, 0.01) var reflections_mix_level: float = 1.0
## Volumen del trazado de rutas (0-10).
@export_range(0.0, 10.0, 0.01) var pathing_mix_level: float = 1.0

# --- Hybrid Reverb ---
@export_group("Hybrid Reverb")
## Multiplicador de ecualización por fuente para la banda baja.
@export_range(0.0, 4.0, 0.1) var reflections_eq_low: float = 1.0
## Multiplicador de ecualización por fuente para la banda media.
@export_range(0.0, 4.0, 0.1) var reflections_eq_mid: float = 1.0
## Multiplicador de ecualización por fuente para la banda alta.
@export_range(0.0, 4.0, 0.1) var reflections_eq_high: float = 1.0
## Muestras antes de que comience la parte paramétrica. -1 = usar valor de simulación.
@export var reflections_delay: int = -1

# --- Spatialization ---
@export_group("Spatialization")
## Anulación por fuente de [member ResonanceRuntimeConfig.direct_binaural]. -1 = Usar Global, 0 = Desactivado (paneo en la ruta dry), 1 = Activado (fuerza HRTF).
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var direct_binaural_override: int = -1
## Anulación por fuente de [member ResonanceRuntimeConfig.reverb_binaural] (decodificación ambisónica wet de reflexión / mezclador). -1 = Usar Global, 0 = Desactivado, 1 = Activado.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var reverb_binaural_override: int = -1
## Anulación por fuente de [member ResonanceRuntimeConfig.pathing_binaural]. -1 = Usar Global, 0 = Desactivado (menos CPU), 1 = Activado.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1") var pathing_binaural_override: int = -1
## Mezcla la salida entre 2D (0, paneo estéreo sin HRTF en la ruta dry) y audio espacial 3D completo (1). Los valores intermedios mezclan ambos (por ejemplo, interfaz de usuario vs. fuentes asociadas al mundo).
@export_range(0.0, 1.0, 0.01) var spatial_blend: float = 1.0
## Codifica la fuente puntual a ambisónicos antes de la binaural.
@export var use_ambisonics_encode: bool = false
## -1 = usar [member ResonanceRuntimeConfig.hrtf_interpolation_bilinear]. 0 = HRTF más cercano (más rápido). 1 = bilineal (movimiento más suave).
## Corrección de perspectiva por fuente. -1 = usar global, 0 = desactivado, 1 = activado.
@export_enum("Use Global:-1", "Nearest:0", "Bilinear:1") var hrtf_interpolation_override: int = -1
var _perspective_correction_override: int = -1
## Corrección de perspectiva por fuente. -1 = usar global, 0 = desactivado, 1 = activado.
@export_enum("Use Global:-1", "Disabled:0", "Enabled:1")
var perspective_correction_override: int = -1:
	get:
		return _perspective_correction_override
	set(v):
		if _perspective_correction_override != v:
			_perspective_correction_override = v
			notify_property_list_changed()
## Factor para mapeo de posición en pantalla (0.5–2.0). Se utiliza cuando la anulación está activada.
@export_range(0.5, 2.0, 0.1) var perspective_factor: float = 1.0


func _validate_property(property: Dictionary) -> void:
	if property.name == "bus_name":
		if bus_override == -1:  # Use Global
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "reverb_bus_name":
		if reverb_bus_override == -1:  # Use Global
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "perspective_factor":
		if perspective_correction_override == 0:  # Disabled
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name in ["air_absorption_low", "air_absorption_mid", "air_absorption_high"]:
		if not air_absorption_enabled or air_absorption_input != 1:  # User Defined
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "attenuation_curve":
		if attenuation_mode != 2:  # Curve
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "current_baked_source":
		if reflections_type != 2:  # Baked Static Source
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "current_baked_listener":
		if reflections_type != 3:  # Baked Static Listener
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "occlusion_value":
		if occlusion_input != 1:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name in ["transmission_low", "transmission_mid", "transmission_high"]:
		if transmission_input != 1:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "reverb_transmission_amount":
		if reverb_transmission_amount_input != 1:  # not User Defined
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "occlusion_samples":
		if occlusion_type_override != 1:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "max_transmission_surfaces":
		if max_transmission_surfaces_override != 1:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name in ["directivity_weight", "directivity_power"]:
		if not directivity_enabled or directivity_input != 0:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY
	elif property.name == "directivity_value":
		if not directivity_enabled or directivity_input != 1:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY


## Resolved direct/path bus: Use Global → [param global_fallback], else [member bus_name] if set.
func get_bus_name_effective(global_fallback: StringName) -> StringName:
	if bus_override == -1:  # Use Global
		return global_fallback
	var custom := bus_name
	return custom if not str(custom).is_empty() else global_fallback


## Resolved reverb bus: Use Global → [param global_fallback], else [member reverb_bus_name] if set.
func get_reverb_bus_name_effective(global_fallback: StringName) -> StringName:
	if reverb_bus_override == -1:  # Use Global
		return global_fallback
	var custom := reverb_bus_name
	return custom if not str(custom).is_empty() else global_fallback


## Creates default player config for sources without one assigned.
static func create_default() -> ResonancePlayerConfig:
	var cfg := ResonancePlayerConfig.new()
	cfg.occlusion_type_override = 2
	cfg.max_transmission_surfaces_override = 0
	return cfg

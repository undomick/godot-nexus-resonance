@tool
@icon("res://addons/nexus_resonance/ui/icons/resonance_config.svg")
extends Resource
class_name ResonanceBakeConfig

## Recurso de GDScript asignado a [member ResonanceProbeVolume.bake_config]. Guárdalo como .tres para preajustes reutilizables. Cuando es nulo en el volumen, se utiliza [method create_default].
## Controla el algoritmo de reflexión, el horneado del trazado de rutas, los pases estáticos de fuente/oyente y la calidad del horneado. [member reflection_type] debe coincidir con [member ResonanceRuntimeConfig.reflection_type] para la reproducción (BakeConfig no tiene TrueAudio Next).
## Los campos de visibilidad del trazado de rutas ([member bake_pathing_vis_range], [member bake_pathing_radius], [member bake_pathing_threshold], etc.) también se leen en tiempo de ejecución desde el primer volumen de sonda cargado que tenga una configuración de horneado.

const Constants = preload("resonance_config_constants.gd")

# --- Reflection ---
@export_group("Reflection")
## Algoritmo de reverberación para el horneado de reflexiones: Convolución (0), Paramétrico (1), Híbrido (2, por defecto). Debe coincidir con [member ResonanceRuntimeConfig.reflection_type] en tiempo de ejecución.
@export_enum("Convolution:0", "Parametric:1", "Hybrid:2")
var reflection_type: int = Constants.REFLECTION_TYPE_HYBRID

# --- Pathing ---
## Hornea la conectividad del trazado de rutas entre sondas. Requerido cuando el trazado de rutas en tiempo real está activado. Cuando es false, las subpropiedades del trazado de rutas son de solo lectura en el inspector.
@export_group("Pathing")
var _pathing_enabled: bool = false
## Hornea la conectividad del trazado de rutas entre sondas. Requerido cuando el trazado de rutas en tiempo real está activado. Cuando es false, las subpropiedades del trazado de rutas son de solo lectura en el inspector.
@export var pathing_enabled: bool:
	get:
		return _pathing_enabled
	set(v):
		if _pathing_enabled != v:
			_pathing_enabled = v
			notify_property_list_changed()
## Distancia máxima en metros (10-2000) para la visibilidad mutua de las sondas durante el horneado del trazado de rutas y el trazado de rutas en tiempo real. Los valores más altos consumen más CPU.
@export_range(10, 2000, 10) var bake_pathing_vis_range: float = 500.0
## Longitud máxima de ruta en metros (10-500) entre sondas para el horneado del trazado de rutas.
@export_range(10, 500, 10) var bake_pathing_path_range: float = 100.0
## Muestras de visibilidad por sonda (4-128) para el horneado del trazado de rutas. Los valores más altos son más suaves pero más lentos; el tiempo de ejecución puede usar [member ResonanceRuntimeConfig.pathing_num_vis_samples] por separado.
@export_range(4, 128, 4) var bake_pathing_num_samples: int = 16
## Radio de la esfera de la sonda en metros (0.1-2.0) para las pruebas de visibilidad del trazado de rutas. Se utiliza en el horneado y en tiempo de ejecución.
@export_range(0.1, 2.0, 0.1) var bake_pathing_radius: float = 0.5
## Fracción de rayos no ocluidos requerida para enlazar sondas (0.01-1.0). Los valores más bajos admiten más rutas e incrementan el tiempo de horneado. Se utiliza en el horneado y en tiempo de ejecución.
@export_range(0.01, 1.0, 0.01) var bake_pathing_threshold: float = 0.1

# --- Additional Bake ---
@export_group("Additional Bake")
## Hornea capas de fuentes estáticas (Static Source) a partir de [member ResonanceProbeVolume.bake_sources] en el volumen propietario (un pase por cada NodePath válido).
@export var static_source_enabled: bool = false
## Hornea capas de oyentes estáticos (Static Listener) a partir de [member ResonanceProbeVolume.bake_listeners] en el volumen propietario (un pase por cada NodePath válido).
@export var static_listener_enabled: bool = false

# --- Quality ---
@export_group("Quality")
## Orden de ambisónicos (1-3) para las respuestas al impulso (IR) de convolución horneadas. Un orden más alto mejora el detalle espacial e incrementa el tamaño y el tiempo de horneado.
@export_range(1, 3, 1) var bake_ambisonics_order: int = 1
## Rayos de reflexión por sonda (256-16384). Más rayos mejoran la calidad y prolongan el horneado.
@export_range(256, 16384, 256) var bake_num_rays: int = 4096
## Rebotes de reflexión por rayo (1-32). Más rebotes prolongan la cola de reverberación y el horneado.
@export_range(1, 32, 1) var bake_num_bounces: int = 4
## Hilos de trabajo de horneado en paralelo (1-64). Afecta únicamente a la duración del horneado; no se incluye en los hashes de sondas desactualizadas.
@export_range(1, 64, 1) var bake_num_threads: int = 2


func _validate_property(property: Dictionary) -> void:
	if (
		property.name
		in [
			"bake_pathing_vis_range",
			"bake_pathing_path_range",
			"bake_pathing_num_samples",
			"bake_pathing_radius",
			"bake_pathing_threshold"
		]
	):
		if not pathing_enabled:
			property["usage"] = property["usage"] | PROPERTY_USAGE_READ_ONLY


## Returns bake params dictionary for C++ set_bake_params.
func get_bake_params() -> Dictionary:
	return {
		"bake_ambisonics_order": bake_ambisonics_order,
		"bake_num_rays": bake_num_rays,
		"bake_num_bounces": bake_num_bounces,
		"bake_num_threads": bake_num_threads,
		"bake_reflection_type": reflection_type,
		"bake_pathing_vis_range": bake_pathing_vis_range,
		"bake_pathing_path_range": bake_pathing_path_range,
		"bake_pathing_num_samples": bake_pathing_num_samples,
		"bake_pathing_radius": bake_pathing_radius,
		"bake_pathing_threshold": bake_pathing_threshold
	}


## Creates default bake config for volumes without one assigned.
static func create_default() -> ResonanceBakeConfig:
	return ResonanceBakeConfig.new()

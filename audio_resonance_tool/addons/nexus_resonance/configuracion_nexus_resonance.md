# Guía de Configuración / Configuration Guide — Nexus Resonance

Esta guía explica en detalle el funcionamiento de cada ajuste disponible en **Ajustes del Proyecto -> Nexus Resonance**.
This guide explains in detail the operation of each setting available in **Project Settings -> Nexus Resonance**.

---

## 1. Depuración y Logs / Debugging and Logs (Logger)

* **Categorías habilitadas / Categories Enabled:**
  * **ES:** Permite activar o desactivar filtros de logs específicos para aislar problemas acústicos (ej. horneado, oclusión, trazado de rutas, reflexiones en tiempo real).
  * **EN:** Enables or disables specific log filters to isolate acoustic issues (e.g. baking, occlusion, pathing, real-time reflections).
* **Enviar salida a depuración / Output To Debug:**
  * **ES:** Redirige los mensajes de log del motor acústico directamente a la pestaña "Salida" del editor de Godot.
  * **EN:** Redirects acoustic engine log messages directly to the Godot editor "Output" tab.
* **Enviar salida a archivo / Output To File:**
  * **ES:** Si está marcado, escribe un reporte detallado en formato estructurado (ndjson) en el almacenamiento del usuario.
  * **EN:** If checked, writes a detailed structured report (ndjson) to user storage.
* **Ruta del archivo / File Path:**
  * **ES:** Define el archivo de guardado del log (por defecto `user://nexus_resonance_log.ndjson`).
  * **EN:** Defines the log save file path (default `user://nexus_resonance_log.ndjson`).
* **Steam Audio detallado / Steam Audio Verbose:**
  * **ES:** Habilita la salida extendida de depuración de la API nativa de Valve Steam Audio (útil para diagnosticar fallos profundos de hardware o inicialización).
  * **EN:** Enables extended debugging output from the native Valve Steam Audio API (useful for diagnosing deep hardware or initialization faults).

---

## 2. Horneado de Escenas / Scene Baking (Bake)

* **Directorio de salida por defecto / Default Output Directory:**
  * **ES:** Ruta del proyecto donde se almacenarán los archivos `.tres` de datos acústicos horneados (por defecto es `res://audio_data/`).
  * **EN:** Project path where baked acoustic data `.tres` files will be stored (defaults to `res://audio_data/`).

---

## 3. Conversión de Animación / Animation Conversion (Editor)

* **Convertir audio automáticamente al guardar / Auto Convert Animation Audio On Save:**
  * **ES:** Si se activa, el plugin convertirá automáticamente las pistas tradicionales de audio de animación que apunten a un `ResonancePlayer` en pistas de llamada de método ('Call Method') al guardar la escena. Esto es necesario para la correcta propagación espacial 3D de sonidos animados.
  * **EN:** If enabled, the plugin will automatically convert traditional animation audio tracks targeting a `ResonancePlayer` into method call ('Call Method') tracks upon saving the scene. This is required for correct 3D spatial propagation of animated sounds.

---

## 4. Formatos de Exportación / Export Formats (Export)

* **Formato del recurso de escena estática / Static Scene Asset Format:**
  * **ES:** Define si la geometría estática fusionada exportada se guardará en formato de texto legible (`.tres`) o binario optimizado (`.res`).
  * **EN:** Defines whether the exported merged static geometry will be saved as human-readable text (`.tres`) or optimized binary (`.res`).
* **Formato de datos de sondas / Probe Data Format:**
  * **ES:** Define si los lotes de sondas acústicas resultantes del horneado se guardarán como texto (`.tres`) o binario (`.res`).
  * **EN:** Defines whether baked acoustic probe batches will be saved as text (`.tres`) or binary (`.res`).

---

## 5. Accesibilidad / Accessibility

* **TTS del Editor / Editor TTS:**
  * **ES:** Activa anuncios mediante voz sintetizada (TTS) en el editor de Godot. Informará por voz del inicio de horneados, cambios de etapa y finalización.
  * **EN:** Enables Text-to-Speech (TTS) narration within the Godot editor. Narrates baking start, stage transitions, and completion.
* **TTS de depuración en ejecución / Runtime Debug TTS:**
  * **ES:** Habilita anuncios de voz a tiempo real durante las pruebas de juego (playtesting) al entrar/salir de zonas acústicas o cambiar el estado de reproducción de fuentes de sonido.
  * **EN:** Enables real-time speech announcements during playtesting when entering/exiting acoustic zones or when sound source playback states change.
* **Voz del TTS / TTS Voice:**
  * **ES:** El identificador de la voz sintetizada del sistema que se utilizará para hablar. Deja en blanco para usar la voz por defecto del sistema.
  * **EN:** The identifier of the system text-to-speech voice to use for narration. Leave blank to use the system default voice.
* **Volumen del TTS / TTS Volume:**
  * **ES:** Ajusta el volumen de la voz sintética de accesibilidad (rango: 0 a 100, por defecto 50).
  * **EN:** Adjusts the accessibility text-to-speech volume level (range: 0 to 100, default 50).
* **Velocidad del TTS / TTS Speed:**
  * **ES:** Ajusta la tasa de reproducción o velocidad de la voz del TTS (rango: 0.1 a 10.0, por defecto 1.0).
  * **EN:** Adjusts the text-to-speech speech rate or speed (range: 0.1 to 10.0, default 1.0).

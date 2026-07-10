# Steam Audio Integration Roadmap & Proposed Improvements

This document outlines the proposed roadmap and architectural improvements to bring the **Nexus Resonance** plugin for Godot 4 closer to the official Steam Audio integrations available in Unity and Unreal Engine.

---

## 1. 3D Viewport Interaction & Manual Probe Editing
* **Current State:** Acoustic probe editing in Godot is restricted to parametric generation (grids) and batch tools in the Inspector (e.g., removing a probe by typing its index).
* **Target State:** Introduce a custom 3D editor viewport spatial tool (`EditorNode3DGizmoPlugin`) that allows sound designers to:
  * Select individual acoustic probes directly inside the 3D viewport.
  * Move probes manually using Godot's translation gizmos to adapt to complex level geometry.
  * Delete specific probes with a keystroke (`Delete` key) and add new ones via mouse clicks.

## 2. Acoustic Material Overlays (Visual Debugging)
* **Current State:** Acoustic materials (`ResonanceMaterial`) are assigned to `ResonanceGeometry` nodes as resources, but there is no visual indicator in the editor showing how sound interacts with the meshes.
* **Target State:** Develop an editor-only debug shader that renders meshes in the 3D viewport color-coded by their acoustic absorption properties:
  * Highly reflective materials (e.g., concrete, metal) colored in cooler tones (blue/cyan).
  * Highly absorbing materials (e.g., carpet, drywall) colored in warmer tones (red/yellow).
  * This allows designers to visually audit the acoustic properties of a level at a glance.

## 3. Dynamic Acoustic Portals (AcousticPortals)
* **Current State:** The plugin relies on `ResonanceDynamicGeometry` to update mesh transforms for dynamic objects. However, there is no dedicated concept of an "acoustic portal" (e.g., doors or windows that block or guide sound propagation).
* **Target State:** Add a custom `ResonanceAcousticPortal` node:
  * This node will act as a dynamic opening with an adjustable open/closed state (from `0.0` to `1.0`).
  * Instead of rebuilding scene geometry, it dynamically alters the pathing simulation (diffraction and transmission) between adjacent rooms, optimizing performance and accuracy for moving doors/windows.

## 4. Middleware Expansion (Wwise Bridge)
* **Current State:** The plugin features a robust bridge for FMOD Studio (`ResonanceFmodEventEmitter` and `ResonanceFMODBridge`).
* **Target State:** Extend support to other industry-standard audio middleware by developing a dedicated bridge for **Audiokinetic Wwise**:
  * Allow synchronization of Wwise Event Emitters with Steam Audio listeners, geometry, and probe volumes in Godot.

## 5. Automated Build Pipeline & Dirty State Tracking
* **Current State:** Probes must be baked manually node by node. Level geometry changes do not trigger warnings, which can lead to outdated acoustic data in built games.
* **Target State:** Implement an automated tracking system:
  * Monitor changes to level meshes. If level geometry changes, mark the corresponding `ResonanceProbeVolume` as "Dirty."
  * Prompt the user to re-bake or offer an automated background baking task prior to project export.

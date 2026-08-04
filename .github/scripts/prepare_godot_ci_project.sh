#!/usr/bin/env bash
# Sync addon SoT into local Godot project root (project/) for CI / headless Godot.
# project/ is gitignored; CI scaffolds a minimal project.godot when missing.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PROJECT_DIR="${GODOT_CI_PROJECT:-$ROOT/project}"
ADDON_SRC="$ROOT/addons/nexus_resonance"
ADDON_DST="$PROJECT_DIR/addons/nexus_resonance"

if [[ ! -d "$ADDON_SRC" ]]; then
  echo "error: addon source missing: $ADDON_SRC" >&2
  exit 1
fi

mkdir -p "$PROJECT_DIR/addons"
rm -rf "$ADDON_DST"
cp -a "$ADDON_SRC" "$ADDON_DST"

# Tracked GUT / smoke scripts live under repo-root test/ (project/ is gitignored).
TEST_SRC="$ROOT/test"
if [[ -d "$TEST_SRC" ]]; then
  mkdir -p "$PROJECT_DIR/test"
  cp -a "$TEST_SRC/." "$PROJECT_DIR/test/"
  echo "Synced $TEST_SRC -> $PROJECT_DIR/test"
fi

if [[ ! -f "$PROJECT_DIR/project.godot" ]]; then
  cat >"$PROJECT_DIR/project.godot" <<'EOF'
; Engine configuration file.
; Generated for CI - local project/ may replace this when present.
config_version=5

[application]

config/name="Nexus Resonance CI"
config/features=PackedStringArray("4.6")

[audio]

buses/default_bus_layout="res://addons/nexus_resonance/default_bus_layout.tres"

[editor_plugins]

enabled=PackedStringArray("res://addons/nexus_resonance/plugin.cfg")
EOF
  echo "Created minimal $PROJECT_DIR/project.godot"
fi

echo "Prepared Godot project at $PROJECT_DIR (addon synced from addons/nexus_resonance)"
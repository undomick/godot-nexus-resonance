# Contributing to Nexus Resonance

Thank you for your interest in contributing. This document provides guidelines for contributing to the project.

## Project Structure

- `addons/nexus_resonance/` - Nexus Resonance plugin (GDScript + GDExtension; source of truth)
- `project/` - Local Godot 4 test project (gitignored; open this in the editor)
- `src/` - C++ GDExtension source (ResonanceServer, Steam Audio integration)

## Development Setup

1. Clone the repository (with submodules):
   ```powershell
   git clone --recurse-submodules <repo-url>
   ```
   Or, if already cloned:
   ```powershell
   git submodule update --init --recursive
   ```
2. Install Steam Audio SDK (downloads from [ValveSoftware/steam-audio](https://github.com/ValveSoftware/steam-audio) releases):
   ```powershell
   python scripts/install_steam_audio.py
   ```
3. Build the GDExtension: `scons`
4. Open `project/` in Godot 4.6 (sync `addons/nexus_resonance/` into `project/addons/nexus_resonance/` first).
5. Enable the Nexus Resonance plugin in Project Settings → Plugins.

### Platform-Specific Builds

```powershell
# Desktop
make build-windows
make build-linux
make build-macos

# Mobile
make build-android
make build-ios        # macOS only; builds pffft and libmysofa for iOS arm64
```

### Library References (Git Submodules)

Libraries are referenced from GitHub as git submodules, not bundled:

- **[godot-cpp](https://github.com/godotengine/godot-cpp)** - Godot C++ bindings (branch 4.5)
- **[Catch2](https://github.com/catchorg/Catch2)** - C++ unit tests (branch v2.x)
- **[pffft](https://github.com/marton78/pffft)** - FFT library (iOS static linking)
- **[libmysofa](https://github.com/hoene/libmysofa)** - HRTF/SOFA file reader (iOS static linking)
- **Steam Audio** - Fetched via `install_steam_audio.py` from [ValveSoftware/steam-audio](https://github.com/ValveSoftware/steam-audio) releases

## Running Tests

### GDScript (GUT)

Unit tests use the [GUT](https://github.com/bitwes/Gut) framework.

**In the editor:** Project → Tools → GUT → Run All (with `res://test/unit` as directory).

Tracked GUT sources live in repo-root [`test/unit/`](../test/unit/) (synced into `project/test/` via `.github/scripts/prepare_godot_ci_project.sh` or a local copy).

**Command line (PowerShell):**
```powershell
# Sync tracked tests + addon into the local Godot project, then:
cd project
.\run_tests.ps1
```

Set `$env:GODOT_PATH` to your Godot executable if it is not in PATH.

### C++ Unit Tests

C++ tests use [Catch2](https://github.com/catchorg/Catch2). Build with tests enabled and run:

```powershell
scons build_tests=1
.\build\tests\nexus_resonance_tests.exe
```

On Linux/macOS: `./build/tests/nexus_resonance_tests`

## Code Style

- **GDScript:** English only in scripts. Use `##` for GDDoc comments on public methods and properties.
- **Naming:** snake_case for variables/functions, PascalCase for classes.
- **Formatting:** Match existing style; use Godot's built-in formatter where applicable.

## Pull Request Process

1. Create a branch from `main` or `master`.
2. Make your changes. Ensure tests pass.
3. Update `docs/CHANGELOG.md` for user-facing changes (add to the upcoming release section; use `[Unreleased]` only when that section exists).
4. Submit a pull request with a clear description.

## Reporting Bugs

Use the [Bug Report](.github/ISSUE_TEMPLATE/bug_report.yml) template. Include Godot version, OS, and relevant output logs.

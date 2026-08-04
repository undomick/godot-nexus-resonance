#!/usr/bin/env python3
"""
Build the Steam Audio FMOD plugin from references/steam-audio-4.8.1/fmod.
Deploys phonon_fmod.dll (Windows) or libphonon_fmod.so (Linux) to the FMOD plugin path.

Requirements:
- CMake 3.17+
- FMOD Studio API (download from https://www.fmod.com/download)
- Steam Audio core (install via: python scripts/install_steam_audio.py)

Environment / Arguments:
- FMOD_ROOT or --fmod-root: Path to FMOD Studio API root (contains api/inc, api/lib)
- STEAM_AUDIO_ROOT or --steam-audio-root: Path to Steam Audio SDK (default: references/steam-audio-4.8.1)
- --deploy: Copy built plugin to addons/nexus_resonance/bin/fmod_plugin/
            or to addons/fmod/lib/ if fmod-gdextension structure exists

Usage:
  python scripts/build_steam_audio_fmod.py --fmod-root "C:/fmodstudioapi" [--deploy]
"""
import os
import sys
import subprocess
import shutil
import argparse

PROJECT_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FMOD_REF = os.path.join(PROJECT_ROOT, "references", "steam-audio-4.8.1", "fmod")
STEAM_AUDIO_ROOT_DEFAULT = os.path.join(PROJECT_ROOT, "src", "lib", "steamaudio")
BUILD_DIR = os.path.join(PROJECT_ROOT, "build", "steam_audio_fmod")
DEPLOY_DIR = os.path.join(PROJECT_ROOT, "addons", "nexus_resonance", "bin", "fmod_plugin")


def main():
    parser = argparse.ArgumentParser(description="Build Steam Audio FMOD plugin")
    parser.add_argument("--fmod-root", help="Path to FMOD Studio API root")
    parser.add_argument("--steam-audio-root", default=STEAM_AUDIO_ROOT_DEFAULT, help="Path to Steam Audio SDK")
    parser.add_argument("--deploy", action="store_true", help="Copy built plugin to addon bin/fmod_plugin")
    args = parser.parse_args()

    fmod_root = args.fmod_root or os.environ.get("FMOD_ROOT")
    if not fmod_root or not os.path.isdir(fmod_root):
        print("Error: FMOD_ROOT or --fmod-root required. Set path to FMOD Studio API (contains api/inc, api/lib).")
        sys.exit(1)

    steam_audio_root = os.path.abspath(args.steam_audio_root)
    if not os.path.isdir(steam_audio_root):
        print(f"Error: Steam Audio root not found: {steam_audio_root}")
        sys.exit(1)

    os.makedirs(BUILD_DIR, exist_ok=True)
    os.chdir(BUILD_DIR)

    # FindSteamAudio expects phonon.h in include, FMOD expects FMODROOT
    fmod_abs = os.path.abspath(fmod_root)
    steam_abs = os.path.abspath(steam_audio_root)
    cmake_args = [
        "-G", "Ninja" if shutil.which("ninja") else "Unix Makefiles",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DFMODROOT={fmod_abs}",
        f"-DFMOD_ROOT={fmod_abs}",
        f"-DSteamAudio_ROOT={steam_abs}",
        FMOD_REF,
    ]

    print("Configuring Steam Audio FMOD plugin...")
    if subprocess.run(["cmake"] + cmake_args).returncode != 0:
        print("CMake configure failed.")
        sys.exit(1)

    print("Building...")
    if subprocess.run(["cmake", "--build", ".", "--config", "Release"]).returncode != 0:
        print("Build failed.")
        sys.exit(1)

    # Locate built plugin
    plugin_name = "phonon_fmod.dll" if sys.platform == "win32" else "libphonon_fmod.so"
    if sys.platform == "darwin":
        plugin_name = "libphonon_fmod.dylib"
    built_plugin = None
    for root, _, files in os.walk(BUILD_DIR):
        for f in files:
            if f == plugin_name or f.endswith(".dll") and "phonon_fmod" in f:
                built_plugin = os.path.join(root, f)
                break
        if built_plugin:
            break
    if not built_plugin or not os.path.isfile(built_plugin):
        # Try Release subdir on Windows
        for sub in ["Release", "Debug", ""]:
            p = os.path.join(BUILD_DIR, sub, plugin_name) if sub else os.path.join(BUILD_DIR, plugin_name)
            if os.path.isfile(p):
                built_plugin = p
                break
    if not built_plugin:
        print(f"Warning: Could not find built {plugin_name}. Check build output.")
    elif args.deploy:
        arch = "windows-x64" if sys.platform == "win32" else ("osx" if sys.platform == "darwin" else "linux-x64")
        deploy_path = os.path.join(DEPLOY_DIR, arch)
        os.makedirs(deploy_path, exist_ok=True)
        dest = os.path.join(deploy_path, plugin_name)
        shutil.copy2(built_plugin, dest)
        print(f"Deployed to {dest}")
        print("Place phonon_fmod in FMOD plugin path (e.g. addons/fmod/lib/<platform>/ for fmod-gdextension).")

    print("Done.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

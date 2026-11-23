#!/usr/bin/env python3
"""
Download and extract Steam Audio SDK binaries for Nexus Resonance.
Uses the official ValveSoftware/steam-audio releases.
Run from project root: python scripts/install_steam_audio.py
"""
import os
import platform
import sys
import zipfile
import urllib.request
import json

STEAM_AUDIO_VERSION = "4.8.1"
GITHUB_API_URL = "https://api.github.com/repos/ValveSoftware/steam-audio/releases/tags/v" + STEAM_AUDIO_VERSION
STEAM_AUDIO_LIB = "src/lib/steamaudio"
ZIP_NAME = "steamaudio.zip"


def get_download_url():
    """Fetch download URL from GitHub API."""
    try:
        req = urllib.request.Request(
            GITHUB_API_URL,
            headers={"Accept": "application/vnd.github.v3+json"},
        )
        with urllib.request.urlopen(req, timeout=10) as r:
            data = json.loads(r.read().decode())
    except Exception as e:
        print(f"Failed to fetch release info: {e}")
        return None

    for asset in data.get("assets", []):
        name = asset.get("name", "")
        if name.endswith(".zip") and ("steamaudio" in name.lower() or "phonon" in name.lower() or "c_api" in name.lower()):
            return asset.get("browser_download_url")
    print("No Steam Audio SDK zip found in release assets.")
    return None


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    os.chdir(root)

    lib_dir = os.path.join(STEAM_AUDIO_LIB, "lib")
    if os.path.isdir(lib_dir):
        # Check if we already have the expected structure
        expected = ["windows-x64", "linux-x64", "osx", "android-armv8", "android-x64", "ios"]
        has_all = all(os.path.isdir(os.path.join(lib_dir, d)) for d in expected)
        if has_all:
            print(f"Steam Audio lib already present at {lib_dir}. Skipping download.")
            return 0

    url = get_download_url()
    if not url:
        sys.exit(1)

    zip_path = os.path.join(STEAM_AUDIO_LIB, ZIP_NAME)
    os.makedirs(STEAM_AUDIO_LIB, exist_ok=True)

    print(f"Downloading Steam Audio v{STEAM_AUDIO_VERSION}...")
    try:
        urllib.request.urlretrieve(url, zip_path)
    except Exception as e:
        print(f"Download failed: {e}")
        sys.exit(1)

    print("Extracting...")
    lib_parent = os.path.dirname(STEAM_AUDIO_LIB)  # src/lib
    dest_path = os.path.join(root, STEAM_AUDIO_LIB)
    with zipfile.ZipFile(zip_path, "r") as z:
        z.extractall(lib_parent)
    # Zip may extract to steamaudio_4.8.1/ or steamaudio/ - ensure final path is steamaudio
    for entry in os.listdir(lib_parent):
        if "steamaudio" in entry.lower() and entry != "steamaudio":
            src = os.path.join(lib_parent, entry)
            if os.path.isdir(src):
                import shutil
                if os.path.exists(dest_path):
                    shutil.rmtree(dest_path)
                shutil.move(src, dest_path)
                break

    try:
        os.remove(zip_path)
    except OSError:
        pass

    print("Done. Steam Audio binaries are in src/lib/steamaudio/lib/")

    if sys.platform.startswith("linux") and platform.machine() in ("aarch64", "arm64"):
        arm64_lib = os.path.join(lib_dir, "linux-arm64", "libphonon.so")
        if os.path.isfile(arm64_lib):
            print("Linux arm64: found src/lib/steamaudio/lib/linux-arm64/libphonon.so")
        else:
            print(
                "\nNOTE: Valve does not ship a linux-arm64 binary in the SDK zip.\n"
                "      The arm64 build must be compiled from source once:\n"
                "        scripts/build_steam_audio_linux_arm64.sh\n"
                "      That script writes src/lib/steamaudio/lib/linux-arm64/libphonon.so."
            )


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env bash
# Build Steam Audio (libphonon.so) for Linux arm64.
#
# Valve ships prebuilt Steam Audio binaries for linux-x64, linux-x86 and the
# Android/iOS/Apple/macOS/Windows targets, but there is no linux-arm64 slice in
# the SDK zip (v4.8.1). The core library itself supports AArch64, so this script
# builds it from source and drops it where the addon expects it:
#
#   src/lib/steamaudio/lib/linux-arm64/libphonon.so
#
# Prerequisites: aarch64 Linux, cmake >= 3.20, ninja, git, a C/C++ toolchain,
# zlib headers, and the libmysofa headers/library
#   Arch / SteamOS:  sudo pacman -S --needed cmake ninja git base-devel zlib libmysofa
#   Debian / Ubuntu: sudo apt install cmake ninja-build git build-essential zlib1g-dev libmysofa-dev
#
# Usage:
#   scripts/build_steam_audio_linux_arm64.sh [--jobs N] [--prefix DIR]
#
# Then build the extension itself:
#   scons platform=linux arch=arm64 target=template_release -j$(nproc)

set -euo pipefail

STEAMAUDIO_VERSION="${STEAMAUDIO_VERSION:-4.8.1}"
BUILD_ROOT="${BUILD_ROOT:-$HOME/.cache/nexus-resonance/steam-audio-arm64}"
PREFIX="$BUILD_ROOT/prefix"
JOBS="$(nproc)"

while [[ $# -gt 0 ]]; do
	case "$1" in
	--jobs) JOBS="$2"; shift 2 ;;
	--prefix) PREFIX="$2"; shift 2 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

die() { echo "error: $*" >&2; exit 1; }

case "$(uname -m)" in
aarch64 | arm64) ;;
*) die "this script targets Linux arm64; detected $(uname -m). Valve's SDK already ships linux-x64, use scripts/install_steam_audio.py for that." ;;
esac

for tool in git cmake ninja; do
	command -v "$tool" >/dev/null || die "missing required tool: $tool"
done

[[ -f /usr/include/mysofa.h ]] || die "missing libmysofa headers (/usr/include/mysofa.h). Install the libmysofa development package."
[[ -f /usr/include/zlib.h ]] || die "missing zlib headers (/usr/include/zlib.h). Install the zlib development package."

mkdir -p "$BUILD_ROOT" "$PREFIX"
cd "$BUILD_ROOT"

# --- pffft ---------------------------------------------------------------
# Steam Audio requires the PFFFT CMake package. The upstream project builds a
# static library by default, which is what we want: it links into libphonon.so
# so the shipped .so has no extra runtime dependency.
if [[ ! -d pffft ]]; then
	git clone --depth 1 https://github.com/marton78/pffft.git
fi
cmake -S pffft -B pffft/build \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$PREFIX/pffft" \
	-DPFFFT_PREFIX="$PREFIX/pffft" >/dev/null
cmake --build pffft/build -j"$JOBS" --target install >/dev/null
PFFFT_INCLUDE="$PREFIX/pffft/include/pffft"
PFFFT_LIBRARY="$PREFIX/pffft/lib/libpffft.a"

# --- flatbuffers ---------------------------------------------------------
# Needed for Steam Audio's scene/serialisation code. flatc is required at build
# time to generate the scene schema headers.
if [[ ! -d flatbuffers ]]; then
	git clone --depth 1 --branch v24.3.25 https://github.com/google/flatbuffers.git
fi
cmake -S flatbuffers -B flatbuffers/build \
	-DCMAKE_BUILD_TYPE=Release \
	-DFLATBUFFERS_BUILD_TESTS=OFF \
	-DFLATBUFFERS_BUILD_FLATC=ON \
	-DCMAKE_INSTALL_PREFIX="$PREFIX/flatbuffers" >/dev/null
cmake --build flatbuffers/build -j"$JOBS" --target install >/dev/null

# --- Steam Audio core ----------------------------------------------------
if [[ ! -d steam-audio ]]; then
	git clone --depth 1 --branch "v$STEAMAUDIO_VERSION" https://github.com/ValveSoftware/steam-audio.git
fi
cd steam-audio

# Steam Audio's own FindMySOFA.cmake hard-codes the zlib include directory to
# <core>/deps/zlib/include, a path that only exists in Valve's CI workspace.
# Point it at the system headers so a source build can configure.
mkdir -p core/deps/zlib/include
ln -sf /usr/include/zlib.h core/deps/zlib/include/zlib.h

cmake -S core -B "build-arm64" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DPFFFT_INCLUDE_DIR="$PFFFT_INCLUDE" \
	-DPFFFT_LIBRARY="$PFFFT_LIBRARY" \
	-DZLIB_INCLUDE_DIR=/usr/include \
	-DZLIB_LIBRARY=/usr/lib/libz.so \
	-DMySOFA_INCLUDE_DIR=/usr/include \
	-DMySOFA_LIBRARY=/usr/lib/libmysofa.so \
	-DFlatBuffers_INCLUDE_DIR="$PREFIX/flatbuffers/include" \
	-DFlatBuffers_EXECUTABLE="$PREFIX/flatbuffers/bin/flatc" \
	-DSTEAMAUDIO_ENABLE_EMBREE=OFF \
	-DSTEAMAUDIO_ENABLE_IPP=OFF \
	-DSTEAMAUDIO_ENABLE_FFTS=OFF \
	-DSTEAMAUDIO_ENABLE_RADEONRAYS=OFF \
	-DSTEAMAUDIO_ENABLE_TRUEAUDIONEXT=OFF \
	-DSTEAMAUDIO_ENABLE_MKL=OFF \
	-DSTEAMAUDIO_BUILD_TESTS=OFF \
	-DSTEAMAUDIO_BUILD_BENCHMARKS=OFF \
	-DSTEAMAUDIO_BUILD_SAMPLES=OFF \
	-DSTEAMAUDIO_BUILD_DOCS=OFF \
	-DSTEAMAUDIO_BUILD_ITESTS=OFF

# Embree, IPP, FFTS, Radeon Rays, TrueAudio Next and MKL are x86-only
# accelerators. On arm64 the default BVH tracer is used instead, so
# ResonanceRuntimeConfig.scene_type must stay 0 (Default).
cmake --build build-arm64 -j"$JOBS"

DEST="$REPO_ROOT/src/lib/steamaudio/lib/linux-arm64"
mkdir -p "$DEST"
cp build-arm64/src/core/libphonon.so "$DEST/libphonon.so"
strip --strip-unneeded "$DEST/libphonon.so"

echo
echo "built $(file -b "$DEST/libphonon.so")"
echo "installed to $DEST"
echo
echo "next:"
echo "  scons platform=linux arch=arm64 target=template_release -j$JOBS"

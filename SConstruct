#!/usr/bin/env python
import os
import shutil
import sys

env = SConscript("src/lib/godot-cpp/SConstruct")

# Project setup
project_name = "nexus_resonance"

# --- BUILD DIRECTORY SETUP ---
# Create a specific build directory based on platform and target (debug/release).
# This keeps the source directory clean of .obj/.o files.
build_dir = "build/{}/{}/".format(env["platform"], env["target"])

# Tell SCons to map the 'src' directory to 'build_dir'.
# duplicate=0 prevents physically copying the .cpp files to the build dir.
env.VariantDir(build_dir, "src", duplicate=0)

# --- INCLUDES ---
# Add the path to the Steam Audio headers (phonon.h)
env.Append(CPPPATH=["src", "src/lib/godot-cpp/include", "src/lib/godot-cpp/gen/include", "src/lib/steamaudio/include"])

# --- LIB PATH ---
env.Append(LIBPATH=["src/lib/godot-cpp/bin"])

# Steam Audio lib path (can be overridden via STEAM_AUDIO_LIB_PATH)
steam_audio_lib = os.environ.get("STEAM_AUDIO_LIB_PATH", "src/lib/steamaudio/lib")

# Output configuration and Steam Audio linking per platform
if env["platform"] == "windows":
    env.Replace(SHLIBSUFFIX=".dll", SHLIBPREFIX="")  # No "lib" prefix (MinGW cross-compile defaults to lib)
    arch_subdir = "windows-x64" if env["arch"] == "x86_64" else "windows-x86"
    env.Append(LIBPATH=[os.path.join(steam_audio_lib, arch_subdir)])
    env.Append(LIBS=["phonon"])

elif env["platform"] == "linux":
    env.Replace(SHLIBSUFFIX=".so")
    arch_subdir = "linux-x64" if env["arch"] == "x86_64" else "linux-x86"
    env.Append(LIBPATH=[os.path.join(steam_audio_lib, arch_subdir)])
    env.Append(LIBS=["phonon"])
    # Optional: reduce symbol export on Linux (create linux_symbols.map if needed)
    symbols_map = "linux_symbols.map"
    if os.path.isfile(symbols_map):
        env.Append(LINKFLAGS=["-Wl,--version-script=%s" % env.File(symbols_map).abspath])

elif env["platform"] == "macos":
    env.Replace(SHLIBSUFFIX=".dylib")
    env.Append(LIBPATH=[os.path.join(steam_audio_lib, "osx")])
    env.Append(LIBS=["phonon"])
    env.Append(LINKFLAGS=["-Wl,-rpath,@loader_path"])

elif env["platform"] == "android":
    env.Replace(SHLIBSUFFIX=".so")
    arch_to_steam = {"arm64": "android-armv8", "x86_64": "android-x64", "arm32": "android-armv7", "x86_32": "android-x86"}
    steam_arch = arch_to_steam.get(env["arch"], "android-armv8")
    env.Append(LIBPATH=[os.path.join(steam_audio_lib, steam_arch)])
    env.Append(LIBS=["phonon"])

elif env["platform"] == "ios":
    env.Append(LIBPATH=[os.path.join(steam_audio_lib, "ios")])
    env.Append(LIBS=["phonon"])

# TARGET PATH (addon source of truth under repo root)
target_base = "addons/nexus_resonance/bin/"
target_name = "nexus_resonance"

# Platform-specific subdirectories
if env["platform"] == "android":
    arch_to_abi = {"arm64": "arm64-v8a", "arm32": "armeabi-v7a", "x86_64": "x86_64", "x86_32": "x86"}
    abi_dir = arch_to_abi.get(env["arch"], "arm64-v8a")
    target_path = os.path.join(target_base, "android", abi_dir, "")
elif env["platform"] == "ios":
    target_path = os.path.join(target_base, "ios", "")
elif env["platform"] == "macos":
    target_path = os.path.join(target_base, "macos", "")
elif env["platform"] == "windows":
    target_path = os.path.join(target_base, "windows", "")
elif env["platform"] == "linux":
    target_path = os.path.join(target_base, "linux", "")
else:
    target_path = target_base

# --- SOURCES ---
# We now look for sources inside the 'build_dir' instead of 'src'.
# SCons knows to look up the actual files in 'src' because of VariantDir.
sources = Glob(build_dir + "*.cpp")

# GDExtension documentation (Godot 4.3+): compile XML doc_classes into the extension
# so Inspector tooltips work for ResonancePlayer, ResonanceProbeVolume, etc.
# Include for all targets: editor, template_debug, template_release (editor may use any)
_target = env.get("target", "template_debug")
if _target in ["editor", "template_debug", "template_release"]:
	try:
		os.makedirs("src/gen", exist_ok=True)
		doc_xml = Glob("addons/nexus_resonance/doc_classes/*.xml")
		doc_data = env.GodotCPPDocData("src/gen/doc_data.gen.cpp", source=doc_xml)
		sources.append(doc_data)
	except (AttributeError, TypeError):
		pass  # GodotCPPDocData not available (older godot-cpp)

if env["platform"] == "ios":
    library = env.StaticLibrary(
        target=target_path + "lib" + target_name,
        source=sources,
    )
else:
    library = env.SharedLibrary(
        target=target_path + target_name,
        source=sources,
    )

# Copy Steam Audio runtime DLLs for Windows so nexus_resonance.dll can load (avoids Error 126: "Das angegebene Modul wurde nicht gefunden")
if env["platform"] == "windows":
    steam_src = os.path.join(steam_audio_lib, "windows-x64" if env["arch"] == "x86_64" else "windows-x86")
    steam_dlls = ["phonon.dll", "GPUUtilities.dll", "TrueAudioNext.dll"]

    def copy_steam_dlls(target, source, env):
        if not os.path.isdir(steam_src):
            print("WARNING: Steam Audio lib not found at %s. Run: python scripts/install_steam_audio.py" % steam_src)
            return 0
        dst = os.path.join(target_base, "windows")
        try:
            os.makedirs(dst, exist_ok=True)
            for dll in steam_dlls:
                src_path = os.path.join(steam_src, dll)
                if not os.path.isfile(src_path):
                    print("WARNING: %s not found in %s" % (dll, steam_src))
                    continue
                try:
                    shutil.copy2(src_path, dst)
                    print("Copied %s -> %s" % (dll, dst))
                except OSError as e:
                    print("WARNING: Could not copy %s into addon bin (file in use?). %s" % (dll, e))
        except OSError as e:
            print("WARNING: Steam Audio runtime copy step failed. %s" % e)
        return 0

    env.AddPostAction(library, env.Action(copy_steam_dlls))

# --- C++ UNIT TESTS (no Godot / no link to phonon; Steam Audio headers only for IPL types in ray tests) ---
build_tests = ARGUMENTS.get("build_tests", "1") == "1"
test_exe = None
if build_tests:
    env_test = env.Clone()
    env_test.Replace(LIBS=[], LIBPATH=[])
    env_test.Append(CPPPATH=["src", "src/lib/catch2/single_include/catch2", "src/lib/steamaudio/include"])
    # Shared src/ compiled into the GDExtension already land in build_dir; tests use a separate obj tree
    # so SCons does not try to reuse the same .obj with two environments (env vs env_test).
    test_obj_dir = "build/tests/obj/{}/{}/".format(env["platform"], env["target"])
    env_test.VariantDir(test_obj_dir, "src", duplicate=0)
    # Paths under build_dir (VariantDir) so .obj files stay in build/, not next to sources in src/.
    test_sources = [
        build_dir + "test/test_main.cpp",
        build_dir + "test/test_ring_buffer.cpp",
        build_dir + "test/test_volume_ramp.cpp",
        build_dir + "test/test_resonance_math_more.cpp",
        build_dir + "test/test_resonance_hash.cpp",
        build_dir + "test/test_bake_ambisonics_order.cpp",
        build_dir + "test/test_handle_manager.cpp",
        build_dir + "test/test_ipl_guard.cpp",
        build_dir + "test/test_custom_scene_invariants.cpp",
        build_dir + "test/test_constants_helpers.cpp",
        build_dir + "test/test_ray_trace_debug_intersect.cpp",
        build_dir + "test/test_physics_ray_batch.cpp",
        build_dir + "test/test_baked_reflection_occlusion.cpp",
        build_dir + "test/test_air_absorption_wet_gating.cpp",
        build_dir + "test/test_baked_reverb_listener_probe.cpp",
        build_dir + "test/test_reflection_last_good_ir.cpp",
        build_dir + "test/test_reflection_ir_fingerprint.cpp",
        build_dir + "test/test_reflection_ir_fingerprint_stub.cpp",
        build_dir + "test/test_ambisonics_decode_orientation.cpp",
        test_obj_dir + "resonance_ambisonics_decode_orientation.cpp",
        build_dir + "test/test_probe_exclusion_math.cpp",
        build_dir + "test/test_probe_batch_lock_order.cpp",
        build_dir + "test/test_pathing_inputs_policy.cpp",
        build_dir + "test/test_source_handle_policy.cpp",
        build_dir + "test/test_static_export_policy.cpp",
    ]
    test_dir = "build/tests"
    test_exe = env_test.Program(os.path.join(test_dir, "nexus_resonance_tests"), test_sources)
    env.Alias("test", test_exe)

# Include compile_commands.json in default target when compiledb=1 (for C++ IntelliSense)
default_targets = [library]
if test_exe:
    default_targets.append(test_exe)
if env.get("compiledb", False):
    default_targets.append("compiledb")
Default(default_targets)
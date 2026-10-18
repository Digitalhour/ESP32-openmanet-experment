"""PlatformIO pre-build hook: reproduce this project's target_add_binary_data() steps.

PlatformIO's espidf integration doesn't run CMake's actual build graph (it
only reads CMake's configure-time output - compile_commands.json /
project_description.json - then recompiles sources itself via SCons), so
add_custom_command()/target_add_binary_data() never runs under `pio run`.
Three places in the dependency tree rely on it to produce a file compiled
straight into the firmware, and each needs this script's own copy of the
generation step, writing output to the exact absolute path CMake's configure
step already recorded as a source (verified against
.pio/build/xiao_esp32s3/compile_commands.json's "file" entries, which is
where PlatformIO discovered all three, since none of this is an
idf_component_register()-registered component with its own entry in
project_description.json):

  1. main/web_ui.html - embeds a *generated* file (minified+gzipped by
     minify_web_ui.py at CMake build time), specifically because
     idf_component_register()'s EMBED_FILES can't take a generated file -
     see main/CMakeLists.txt's own comment and CLAUDE.md's "Things that
     look like cleanup but aren't". main/CMakeLists.txt is left untouched;
     it's still what `idf.py build` and CI use.

  2/3. managed_components/morsemicro__halow/components/firmware/CMakeLists.txt
     (a *vendored* dependency, not ours to edit) does the same for the HaLow
     radio firmware and this hardware's BCF calibration blob, resolved via
     sdkconfig.defaults' CONFIG_MM_CHIP_MM6108 / CONFIG_MM_BCF_FILE (see
     CLAUDE.md on why the BCF is fixed to the FGH100M-H's US calibration).
     The BCF ships as a raw .bin and is converted to .mbin by Morse's own
     convert-bin-to-mbin.py first; the firmware ships pre-built as .mbin.
     This is hardcoded to that one file pair rather than reimplementing the
     vendor's Kconfig-driven search, since this project targets exactly one
     board - but re-check it against the vendored CMakeLists.txt whenever
     `pio pkg update`/the component manager bumps morsemicro/halow, since a
     new version could change file names or the conversion step without any
     CI coverage of this PlatformIO-only path to catch it.

Uses the same tools/cmake/scripts/data_file_embed_asm.cmake ESP-IDF itself
uses for this - see main/CMakeLists.txt's comment on target_add_binary_data()
calling it internally, and _embed_files.py's FileToAsm builder in this same
platform package, which drives it identically for the standard EMBED_FILES
path this project doesn't use.
"""

import os
import subprocess
import sys

Import("env")

PROJECT_DIR = env.subst("$PROJECT_DIR")
BUILD_DIR = env.subst("$BUILD_DIR")

HALOW_DIR = os.path.join(PROJECT_DIR, "managed_components", "morsemicro__halow")
MM_IOT_SDK = os.path.join(HALOW_DIR, "components", "mm-iot-sdk")

cmake_exe = os.path.join(
    env.PioPlatform().get_package_dir("tool-cmake"), "bin", "cmake"
)
embed_asm_script = os.path.join(
    env.PioPlatform().get_package_dir("framework-espidf"),
    "tools", "cmake", "scripts", "data_file_embed_asm.cmake",
)


def embed_binary_data(data_file, source_file, variable_basename):
    # Mirrors what target_add_binary_data() does internally - see
    # data_file_embed_asm.cmake's own header comment and
    # main/CMakeLists.txt's citation of it.
    subprocess.check_call([
        cmake_exe,
        "-DDATA_FILE=%s" % data_file,
        "-DSOURCE_FILE=%s" % source_file,
        "-DFILE_TYPE=BINARY",
        "-DVARIABLE_BASENAME=%s" % variable_basename,
        "-P", embed_asm_script,
    ])


os.makedirs(BUILD_DIR, exist_ok=True)

# 1. main/web_ui.html: minify+gzip (matches main/CMakeLists.txt's
# add_custom_command), then embed. RENAME_TO there is "web_ui_html", so the
# linked symbols are _binary_web_ui_html_start/_end either way -
# main/web_ui.c:29-30 needs no PlatformIO-specific branch.
web_ui_src = os.path.join(PROJECT_DIR, "main", "web_ui.html")
web_ui_minifier = os.path.join(PROJECT_DIR, "main", "minify_web_ui.py")
web_ui_gz = os.path.join(BUILD_DIR, "web_ui.min.html.gz")

subprocess.check_call(
    [sys.executable, web_ui_minifier, "--gzip", web_ui_src, web_ui_gz]
)
embed_binary_data(web_ui_gz, web_ui_gz + ".S", "web_ui_html")

# 2. HaLow radio firmware: ships pre-built as .mbin, no conversion needed -
# managed_components/morsemicro__halow/components/firmware/CMakeLists.txt's
# find_binary_blob() finds this exact file for CONFIG_MM_CHIP_MM6108.
fw_mbin = os.path.join(MM_IOT_SDK, "framework", "morsefirmware", "mm6108.mbin")
embed_binary_data(
    fw_mbin, os.path.join(BUILD_DIR, "mm6108.mbin.S"), "firmware_binary"
)

# 3. BCF calibration blob for this hardware (see CLAUDE.md on why this is a
# fixed, non-configurable choice): ships as a raw .bin, converted to .mbin
# by Morse's own script first, exactly as find_binary_blob()'s
# .bin -> .mbin branch does.
bcf_bin = os.path.join(
    HALOW_DIR, "components", "firmware", "morse-firmware", "bcf", "quectel",
    "bcf_fgh100mhaamd.bin",
)
bcf_mbin = os.path.join(BUILD_DIR, "bcf_fgh100mhaamd.bin.mbin")
convert_bin_to_mbin = os.path.join(
    MM_IOT_SDK, "framework", "tools", "buildsystem", "convert-bin-to-mbin.py"
)

subprocess.check_call([
    sys.executable, convert_bin_to_mbin,
    "-t", "1,2:2", "-o", bcf_mbin, bcf_bin,
])
embed_binary_data(bcf_mbin, bcf_mbin + ".S", "bcf_binary")

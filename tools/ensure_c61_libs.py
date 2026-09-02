# Integrate the ESP32-C61 precompiled libraries into the Arduino framework
# package before the framework build script runs.
#
# pioarduino ships the ESP32-C61 precompiled libraries in a separate package
# (framework-arduino-c61-skeleton-lib) and only merges them into
# framework-arduinoespressif32-libs on the ESP-IDF hybrid build path
# (builder/frameworks/espidf.py). A plain Arduino build never runs that merge,
# so a clean environment fails with:
#   missing SConscript file '.../framework-arduinoespressif32-libs/esp32c61/pioarduino-build.py'
# This pre-script performs the same copytree the platform does on the hybrid
# path, and is a no-op once the libraries are in place.

import os
import shutil

Import("env")

board = env.BoardConfig()
if board.get("build.mcu", "") == "esp32c61":
    platform = env.PioPlatform()
    libs_dir = platform.get_package_dir("framework-arduinoespressif32-libs")
    skeleton_dir = platform.get_package_dir("framework-arduino-c61-skeleton-lib")
    if libs_dir and skeleton_dir:
        dst = os.path.join(libs_dir, "esp32c61")
        src = os.path.join(skeleton_dir, "esp32c61")
        # The build script is the file the framework SConscript needs; treat its
        # absence as "not integrated" so an interrupted copy is also repaired.
        if os.path.isdir(src) and not os.path.exists(
            os.path.join(dst, "pioarduino-build.py")
        ):
            print("Integrating framework-arduino-c61-skeleton-lib into "
                  "framework-arduinoespressif32-libs (esp32c61)")
            shutil.copytree(src, dst, dirs_exist_ok=True)

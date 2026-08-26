cmake_minimum_required(VERSION 3.22)
project(savr LANGUAGES CXX ASM)

# Developer builds keep full logging, the FPS panel chord and the full
# 4-digit build version on screen. Player/kit builds leave this OFF.
option(SAVR_DEV "Developer build: logging, FPS panel, full version" OFF)
if(SAVR_DEV)
    add_compile_definitions(SAVR_DEV=1)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENXR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/vendor/openxr)

# The OpenXR loader binary is not committed to the public source kit. The
# build wizard downloads the exact Khronos 1.1.43 AAR, verifies it, and
# passes its arm64 library via OPENXR_LOADER_SO (flag or environment).
# A tree that does carry the loader under vendor/openxr/lib/<abi>/ is used
# as-is when nothing is passed.
set(OPENXR_LOADER_SO "" CACHE FILEPATH
    "Path to the Khronos Android arm64-v8a libopenxr_loader.so")
if(NOT OPENXR_LOADER_SO AND DEFINED ENV{SAVR_OPENXR_LOADER_SO})
    set(OPENXR_LOADER_SO "$ENV{SAVR_OPENXR_LOADER_SO}")
endif()
if(NOT OPENXR_LOADER_SO)
    set(OPENXR_LOADER_SO "${OPENXR_DIR}/lib/${ANDROID_ABI}/libopenxr_loader.so")
endif()
if(NOT EXISTS "${OPENXR_LOADER_SO}")
    message(FATAL_ERROR
        "OPENXR_LOADER_SO must point to the verified Khronos 1.1.43 "
        "arm64-v8a libopenxr_loader.so. Run BUILD_AND_INSTALL or pass "
        "-DOPENXR_LOADER_SO=<path>.")
endif()

add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION "${OPENXR_LOADER_SO}"
)

add_library(savr SHARED
    src/main.cpp
    src/Appearance.cpp
    src/Symbols.cpp
    src/SetupMapDrawDistanceBridge.S
    src/EyeDepth.cpp
    src/VrCamera.cpp
    src/VrFire.cpp
    src/Xr.cpp
    src/Cheats.cpp
    src/HudSettings.cpp
    src/Locomotion.cpp
    src/Driving.cpp
    src/GestureMove.cpp
    src/GraphicsEffects.cpp
    src/Holster.cpp
    src/Melee.cpp
    src/PerfTelemetry.cpp
    src/PhysicalWeapon.cpp
    src/ScopeAim.cpp
    src/Throwable.cpp
    src/TrafficCensus.cpp
    src/Weapon.cpp
    src/Calib.cpp
)

target_include_directories(savr PRIVATE src ${OPENXR_DIR}/include)

# -fvisibility=hidden keeps our own symbol table down to the JNI entry points, so
# nothing of ours can accidentally shadow a game symbol during resolution.
target_compile_options(savr PRIVATE
    -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
    -fvisibility=hidden
    $<$<CONFIG:Release>:-O2>
)

find_library(log-lib log)
find_library(android-lib android)
find_library(egl-lib EGL)
find_library(gles-lib GLESv3)

target_link_libraries(savr PRIVATE
    openxr_loader
    ${log-lib} ${android-lib} ${egl-lib} ${gles-lib}
)

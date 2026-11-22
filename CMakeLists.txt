cmake_minimum_required(VERSION 3.20)
project(Astralis VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(GH_BUILD_GAME "Build the game executable (needs raylib)" ON)
option(GH_WINDOWS_LEGACY "Build for Windows Vista/7+ (OpenGL 2.1, Vista subsystem 6.0)" OFF)
# CPU ISA target for x86 / x86-64 builds:
#   AUTO - SSE2 on 32-bit x86, SSE4 on 64-bit x86-64
#   SSE2 - pure SSE2 (required for 32-bit x86 builds; compatible with all SSE2 CPUs)
#   SSE4 - x86-64 SSE4.2 without AVX2 (compatible with Intel Core i5 3rd gen / Ivy Bridge and older x64 CPUs)
#   AVX2 - x86-64 AVX2 + FMA (Haswell 4th gen / Ryzen and newer)
set(GH_CPU_ISA "AUTO" CACHE STRING "Target x86 CPU ISA (AUTO, SSE2, SSE4, AVX2)")
# ASM note renderer options: x86 SSE2 is enabled; x86-64 SSE4 and AVX2 are disabled by default.
option(GH_ASM_SSE4 "Enable x86-64 SSE4 assembly note renderer (disabled by default)" OFF)
option(GH_ASM_AVX2 "Enable AVX2 assembly note renderer (disabled by default)" OFF)

if(ANDROID)
  option(GH_BUILD_TESTS "Build the headless test executable" OFF)
else()
  option(GH_BUILD_TESTS "Build the headless test executable" ON)
endif()

find_package(Threads REQUIRED)

# Static MSVC runtime: the game runs without installing the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Determine target architecture & ISA compiler flags.
set(GH_IS_X86_64 OFF)
set(GH_IS_X86_32 OFF)
if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|x64|X64)$")
  set(GH_IS_X86_64 ON)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 4 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86|x86|X86|i86pc)$")
  set(GH_IS_X86_32 ON)
endif()

string(TOUPPER "${GH_CPU_ISA}" GH_CPU_ISA_UPPER)
if(GH_IS_X86_32)
  # 32-bit build is strictly pure SSE2 (no SSE3/SSE4/AVX).
  set(GH_CPU_ISA_EFFECTIVE "SSE2")
elseif(GH_IS_X86_64)
  if(GH_CPU_ISA_UPPER STREQUAL "AVX2")
    set(GH_CPU_ISA_EFFECTIVE "AVX2")
  elseif(GH_CPU_ISA_UPPER STREQUAL "SSE2")
    set(GH_CPU_ISA_EFFECTIVE "SSE2")
  else()
    set(GH_CPU_ISA_EFFECTIVE "SSE4")
  endif()
else()
  set(GH_CPU_ISA_EFFECTIVE "NATIVE")
endif()

set(GH_ISA_FLAGS)
if(GH_IS_X86_32)
  if(MSVC)
    list(APPEND GH_ISA_FLAGS /arch:SSE2)
  else()
    # -mstackrealign prevents 32-bit Windows SSE2 stack alignment crashes (movaps) inside Win32/GLFW/OpenGL callbacks.
    list(APPEND GH_ISA_FLAGS -march=pentium4 -msse2 -mfpmath=sse -mstackrealign -mno-sse3 -mno-ssse3 -mno-sse4 -mno-avx -mno-avx2)
  endif()
elseif(GH_IS_X86_64)
  if(GH_CPU_ISA_EFFECTIVE STREQUAL "AVX2")
    if(MSVC)
      list(APPEND GH_ISA_FLAGS /arch:AVX2)
    else()
      list(APPEND GH_ISA_FLAGS -mavx2 -mfma)
    endif()
  elseif(GH_CPU_ISA_EFFECTIVE STREQUAL "SSE4")
    if(NOT MSVC)
      # SSE4.2 without AVX/AVX2: fast and safe on Intel Core i5 3rd gen (Ivy Bridge) and older SSE4 CPUs.
      list(APPEND GH_ISA_FLAGS -msse4.2 -mno-avx -mno-avx2 -mno-fma)
    endif()
  else()
    if(NOT MSVC)
      list(APPEND GH_ISA_FLAGS -msse2 -mno-sse3 -mno-sse4 -mno-avx -mno-avx2)
    endif()
  endif()
endif()
message(STATUS "Astralis target CPU ISA: ${GH_CPU_ISA_EFFECTIVE} (flags: ${GH_ISA_FLAGS})")

function(gh_apply_isa target_name)
  if(GH_ISA_FLAGS AND TARGET ${target_name})
    target_compile_options(${target_name} PRIVATE ${GH_ISA_FLAGS})
  endif()
endfunction()

include(FetchContent)

# The complete source package ships every library in third_party/: use those
# copies instead of downloading (the game then builds without internet).
foreach(dep IN ITEMS lua miniz pl_mpeg raylib)
  string(TOUPPER ${dep} dep_upper)
  if(NOT FETCHCONTENT_SOURCE_DIR_${dep_upper} AND IS_DIRECTORY ${CMAKE_SOURCE_DIR}/third_party/${dep})
    set(FETCHCONTENT_SOURCE_DIR_${dep_upper} ${CMAKE_SOURCE_DIR}/third_party/${dep})
    message(STATUS "Using third_party/${dep}")
  endif()
endforeach()

# ---- Lua 5.4 (mod scripts) -----------------------------------------------------
FetchContent_Declare(lua
  GIT_REPOSITORY https://github.com/lua/lua.git
  GIT_TAG v5.4.7
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(lua)
set(GH_LUA_SOURCES)
foreach(f lapi lauxlib lbaselib lcode lcorolib lctype ldebug ldo ldump lfunc lgc llex lmathlib lmem
          lobject lopcodes lparser lstate lstring lstrlib ltable ltablib ltm lundump lutf8lib lvm lzio)
  list(APPEND GH_LUA_SOURCES ${lua_SOURCE_DIR}/${f}.c)
endforeach()
set_source_files_properties(${GH_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
add_library(gh_lua STATIC ${GH_LUA_SOURCES})
target_include_directories(gh_lua PUBLIC ${lua_SOURCE_DIR})
target_compile_definitions(gh_lua PRIVATE LUA_UCID)
if(MSVC)
  target_compile_options(gh_lua PRIVATE /TP /W0)
  target_compile_definitions(gh_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(gh_lua PRIVATE -w)
endif()
gh_apply_isa(gh_lua)

# ---- miniz (installing mods from .zip and .ghc compression) --------------------
FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 3.0.2
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-add)
FetchContent_MakeAvailable(miniz)
add_library(gh_miniz STATIC
  ${miniz_SOURCE_DIR}/miniz.c ${miniz_SOURCE_DIR}/miniz_tdef.c
  ${miniz_SOURCE_DIR}/miniz_tinfl.c ${miniz_SOURCE_DIR}/miniz_zip.c)
target_include_directories(gh_miniz PUBLIC ${miniz_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/cmake)
if(MSVC)
  target_compile_definitions(gh_miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(gh_miniz PRIVATE -w)
endif()
gh_apply_isa(gh_miniz)

# ---- pl_mpeg (MPEG-1 video for cutscenes) --------------------------------------
FetchContent_Declare(pl_mpeg
  GIT_REPOSITORY https://github.com/phoboslab/pl_mpeg.git
  GIT_TAG c871f2be022ece7ef4f64230b4fb8e1fb9eb6023
  SOURCE_SUBDIR do-not-add)
FetchContent_MakeAvailable(pl_mpeg)

# ---- engine core: pure C++, no window/audio dependency -------------------------
add_library(gh_core STATIC
  src/json.cpp
  src/chart.cpp
  src/chart_stream.cpp
  src/chart_binary.cpp
  src/basegame.cpp
  src/builtin_events.cpp
  src/shader_glsl.cpp
  src/effects.cpp
  src/hscript.cpp
  src/hscript_bridge.cpp
  src/session.cpp
  src/synth.cpp
  src/composer.cpp
  src/psych.cpp
  src/scene.cpp
  src/song_scan.cpp
  src/flp_import.cpp
  src/lua_host.cpp
  src/replay.cpp
  src/mod_install.cpp
  src/note_raster.cpp
  src/frame_pacer.cpp
  src/spectrum.cpp
  src/android_encoder.cpp
)
target_include_directories(gh_core PUBLIC src)

# The note renderer's inner loop in assembly:
#   - x86 SSE2 (enabled on both 32-bit x86 and 64-bit x86-64)
#   - x86-64 SSE4 (disabled by default via GH_ASM_SSE4=OFF)
#   - AVX2 (disabled by default via GH_ASM_AVX2=OFF)
#   - ARM64 NEON (Android, Apple Silicon, ARM Linux)
if(GH_IS_X86_64)
  if(MSVC)
    enable_language(ASM_MASM)
    target_sources(gh_core PRIVATE src/asm/blend_x64.asm)
  else()
    enable_language(ASM)
    target_sources(gh_core PRIVATE src/asm/blend_x64.S)
  endif()
  target_compile_definitions(gh_core PRIVATE
    GH_X86_ASM=1
    GH_ASM_SSE4=$<BOOL:${GH_ASM_SSE4}>
    GH_ASM_AVX2=$<BOOL:${GH_ASM_AVX2}>)
  message(STATUS "Note renderer: x86-64 assembly (SSE2 enabled; SSE4=${GH_ASM_SSE4}, AVX2=${GH_ASM_AVX2})")
elseif(GH_IS_X86_32 AND NOT MSVC)
  enable_language(ASM)
  target_sources(gh_core PRIVATE src/asm/blend_x86.S)
  target_compile_definitions(gh_core PRIVATE GH_X86_ASM=1 GH_ASM_SSE4=0 GH_ASM_AVX2=0)
  message(STATUS "Note renderer: 32-bit x86 pure SSE2 assembly")
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT MSVC AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  enable_language(ASM)
  target_sources(gh_core PRIVATE src/asm/blend_arm64.S)
  target_compile_definitions(gh_core PRIVATE GH_ARM_ASM=1)
  message(STATUS "Note renderer: ARM64 assembly")
endif()

target_link_libraries(gh_core PUBLIC Threads::Threads gh_lua gh_miniz)
if(MSVC)
  target_compile_options(gh_core PRIVATE /W3 /utf-8)
else()
  target_compile_options(gh_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()
gh_apply_isa(gh_core)

if(GH_BUILD_TESTS)
  add_executable(gh_tests tests/tests.cpp tests/psych_tests.cpp tests/scan_tests.cpp tests/raster_tests.cpp tests/pacer_tests.cpp)
  target_link_libraries(gh_tests PRIVATE gh_core)
  gh_apply_isa(gh_tests)
  enable_testing()
  add_test(NAME core COMMAND gh_tests)
endif()

# ---- the game ----------------------------------------------------------------
if(GH_BUILD_GAME)
  set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(BUILD_GAMES OFF CACHE BOOL "" FORCE)
  set(CUSTOMIZE_BUILD OFF CACHE BOOL "" FORCE)
  if(ANDROID)
    set(PLATFORM "Android" CACHE STRING "" FORCE)
  elseif(WIN32 AND GH_WINDOWS_LEGACY)
    set(OPENGL_VERSION "2.1" CACHE STRING "" FORCE)  # older Vista/7 GPUs (Intel GMA/HD 2000-4000, DX9/10 cards)
  endif()
  FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG 5.5
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(raylib)
  gh_apply_isa(raylib)
  if(TARGET glfw)
    gh_apply_isa(glfw)
  endif()
  if(EXISTS ${raylib_SOURCE_DIR}/src/external/dr_mp3.h)
    set(GH_RAYLIB_EXTERNAL ${raylib_SOURCE_DIR}/src/external)
  else()
    set(GH_RAYLIB_EXTERNAL ${raylib_SOURCE_DIR}/external)
  endif()
  set_source_files_properties(src/audio_decode.cpp PROPERTIES INCLUDE_DIRECTORIES ${GH_RAYLIB_EXTERNAL})
  target_compile_definitions(raylib PRIVATE SUPPORT_CUSTOM_FRAME_CONTROL=1)

  # Options > Audio latency: patch raudio.c to allow custom buffer period.
  set(gh_raudio ${raylib_SOURCE_DIR}/src/raudio.c)
  file(READ ${gh_raudio} gh_src)
  if(NOT gh_src MATCHES "GhAudioPeriodMs")
    set(gh_cb "    config.dataCallback = OnSendAudioDataToDevice;")
    set(gh_init "void InitAudioDevice(void)\n{")
    string(FIND "${gh_src}" "${gh_cb}" gh_at1)
    string(FIND "${gh_src}" "${gh_init}" gh_at2)
    if(gh_at1 EQUAL -1 OR gh_at2 EQUAL -1)
      message(FATAL_ERROR "Could not patch ${gh_raudio} (raylib 5.5 expected)")
    endif()
    string(REPLACE "${gh_cb}"
      "${gh_cb}\n    config.periodSizeInMilliseconds = GhAudioPeriodMs;  // Astralis: Options > Audio latency\n    config.periods = GhAudioPeriods;"
      gh_src "${gh_src}")
    string(REPLACE "${gh_init}"
      "unsigned int GhAudioPeriodMs = 0, GhAudioPeriods = 0;  // Astralis: the audio buffer (0: miniaudio's default)\n\n${gh_init}"
      gh_src "${gh_src}")
    file(WRITE ${gh_raudio} "${gh_src}")
    message(STATUS "Patched raylib's raudio.c: the game picks the audio buffer")
  endif()
  file(READ ${gh_raudio} gh_src)
  if(NOT gh_src MATCHES "GhStopBeforeClose")
    set(gh_close "        ma_mutex_uninit(&AUDIO.System.lock);\n        ma_device_uninit(&AUDIO.System.device);")
    string(FIND "${gh_src}" "${gh_close}" gh_at)
    if(gh_at EQUAL -1)
      message(FATAL_ERROR "Could not patch CloseAudioDevice() in ${gh_raudio} (raylib 5.5 expected)")
    endif()
    string(REPLACE "${gh_close}"
      "        // GhStopBeforeClose (Astralis): on Android stopped first; the lock last\n#if defined(__ANDROID__)\n        ma_device_stop(&AUDIO.System.device);\n        ma_sleep(20);\n#endif\n        ma_device_uninit(&AUDIO.System.device);\n        ma_mutex_uninit(&AUDIO.System.lock);"
      gh_src "${gh_src}")
    file(WRITE ${gh_raudio} "${gh_src}")
    message(STATUS "Patched raylib's raudio.c: the audio device stops before it closes")
  endif()
  if(ANDROID)
    set(gh_rcore ${raylib_SOURCE_DIR}/src/platforms/rcore_android.c)
    file(READ ${gh_rcore} gh_src)
    if(NOT gh_src MATCHES "GhFollowWindowSize")
      file(READ ${CMAKE_SOURCE_DIR}/cmake/raylib_android_resize.c gh_fn)
      set(gh_def "static void AndroidCommandCallback(struct android_app *app, int32_t cmd)\n{")
      string(REPLACE "${gh_def}" "${gh_fn}${gh_def}" gh_src "${gh_src}")
      string(REPLACE "        case APP_CMD_CONFIG_CHANGED:"
        "        case APP_CMD_WINDOW_RESIZED:\n        case APP_CMD_CONTENT_RECT_CHANGED:\n        case APP_CMD_CONFIG_CHANGED:"
        gh_src "${gh_src}")
      string(REPLACE "            // Check screen orientation here!" "            GhFollowWindowSize(app);" gh_src "${gh_src}")
      string(REPLACE "platform.contextRebindRequired = false;"
        "platform.contextRebindRequired = false;\n                    GhFollowWindowSize(app);" gh_src "${gh_src}")
      string(REGEX MATCHALL "GhFollowWindowSize\\(app\\)" gh_calls "${gh_src}")
      list(LENGTH gh_calls gh_count)
      if(NOT gh_count EQUAL 2 OR NOT gh_src MATCHES "case APP_CMD_WINDOW_RESIZED")
        message(FATAL_ERROR "Could not patch ${gh_rcore} (raylib 5.5 expected)")
      endif()
      file(WRITE ${gh_rcore} "${gh_src}")
      message(STATUS "Patched raylib's rcore_android.c to follow window size changes")
    endif()
  endif()

  set(GH_GAME_SOURCES
    src/main.cpp
    src/app.cpp
    src/app_icon.cpp
    src/audio.cpp
    src/audio_decode.cpp
    src/input.cpp
    src/settings.cpp
    src/library.cpp
    src/gfx.cpp
    src/characters.cpp
    src/stage.cpp
    src/states_menu.cpp
    src/states_options.cpp
    src/state_play.cpp
    src/play_psych.cpp
    src/scene_draw.cpp
    src/video.cpp
    src/state_results.cpp
    src/state_editor.cpp
    src/editor_preview.cpp
    src/state_editors.cpp
    src/state_modpacks.cpp
    src/process.cpp
    src/render_mode.cpp
    src/platform.cpp
    src/folder_picker.cpp
    src/psych_ui.cpp
    src/note_renderer.cpp
    src/sysinfo.cpp
    src/touch_ui.cpp
    src/visualizer.cpp
    src/check_scripts.cpp
    src/performance.cpp
  )
  if(ANDROID)
    add_library(astralis SHARED ${GH_GAME_SOURCES})
    target_include_directories(astralis PRIVATE ${ANDROID_NDK}/sources/android/native_app_glue)
    target_link_libraries(astralis PRIVATE gh_core raylib android log EGL GLESv2 OpenSLES mediandk m)
    target_link_options(astralis PRIVATE -u ANativeActivity_onCreate -Wl,--no-undefined -Wl,-z,max-page-size=16384)
  else()
    add_executable(astralis WIN32 ${GH_GAME_SOURCES})
  endif()
  if(WIN32)
    if(MSVC)
      target_sources(astralis PRIVATE src/windows/godshand.manifest)
      set(GH_ICON_RC "${CMAKE_BINARY_DIR}/astralis_icon.rc")
      file(CONFIGURE OUTPUT "${GH_ICON_RC}" CONTENT "GLFW_ICON ICON \"${CMAKE_SOURCE_DIR}/src/windows/godshand.ico\"\n")
      target_sources(astralis PRIVATE "${GH_ICON_RC}")
      set_source_files_properties("${GH_ICON_RC}" PROPERTIES OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/src/windows/godshand.ico")
    else()
      set(GH_WIN_RC "${CMAKE_BINARY_DIR}/astralis_win.rc")
      file(CONFIGURE OUTPUT "${GH_WIN_RC}" CONTENT
        "1 24 \"${CMAKE_SOURCE_DIR}/src/windows/godshand.manifest\"\nGLFW_ICON ICON \"${CMAKE_SOURCE_DIR}/src/windows/godshand.ico\"\n")
      target_sources(astralis PRIVATE "${GH_WIN_RC}")
      set_source_files_properties("${GH_WIN_RC}" PROPERTIES OBJECT_DEPENDS
        "${CMAKE_SOURCE_DIR}/src/windows/godshand.manifest;${CMAKE_SOURCE_DIR}/src/windows/godshand.ico")
    endif()
  endif()
  target_link_libraries(astralis PRIVATE gh_core raylib)
  target_compile_definitions(astralis PRIVATE GH_VERSION="${PROJECT_VERSION}" GH_BUILD_ISA="${GH_CPU_ISA_EFFECTIVE}")
  target_include_directories(astralis PRIVATE ${pl_mpeg_SOURCE_DIR})
  gh_apply_isa(astralis)
  if(WIN32 AND GH_WINDOWS_LEGACY)
    target_compile_definitions(gh_core PUBLIC _WIN32_WINNT=0x0600 WINVER=0x0600)
    target_compile_definitions(raylib PRIVATE _WIN32_WINNT=0x0600 WINVER=0x0600)
    target_compile_definitions(astralis PRIVATE _WIN32_WINNT=0x0600 WINVER=0x0600)
  endif()
  if(MSVC)
    target_compile_options(astralis PRIVATE /W3 /utf-8)
    target_link_options(astralis PRIVATE /ENTRY:mainCRTStartup)
    if(GH_WINDOWS_LEGACY)
      target_link_options(astralis PRIVATE /SUBSYSTEM:WINDOWS,6.0)
    endif()
  else()
    target_compile_options(astralis PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
  endif()
  if(MINGW)
    # Fully static runtime including libwinpthread so the binary launches on any Windows PC without extra DLLs.
    target_link_options(astralis PRIVATE -static -static-libgcc -static-libstdc++)
    target_link_libraries(astralis PRIVATE -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
    if(GH_WINDOWS_LEGACY)
      target_link_options(astralis PRIVATE -Wl,--subsystem,windows:6.0)
    endif()
  endif()
  if(APPLE)
    target_link_libraries(astralis PRIVATE "-framework IOKit" "-framework Cocoa" "-framework OpenGL" "-framework CoreFoundation")
    target_link_options(astralis PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${CMAKE_SOURCE_DIR}/cmake/Info.plist")
  endif()
  if(UNIX AND NOT APPLE AND NOT ANDROID)
    target_link_libraries(astralis PRIVATE ${CMAKE_DL_LIBS})
  endif()
  if(WIN32)
    target_link_libraries(astralis PRIVATE shell32 ole32 opengl32 advapi32)
  endif()

  if(NOT ANDROID)
    add_custom_command(TARGET astralis POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/assets $<TARGET_FILE_DIR:astralis>/assets
      COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/songs $<TARGET_FILE_DIR:astralis>/songs
      COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:astralis>/mods)

    install(TARGETS astralis RUNTIME DESTINATION .)
    install(DIRECTORY assets songs DESTINATION .)
    install(FILES README.md LICENSE DESTINATION .)
    install(DIRECTORY licenses DESTINATION .)
  endif()
endif()

if(GH_BUILD_TESTS AND TARGET raylib)
  target_sources(gh_tests PRIVATE tests/audio_tests.cpp src/audio.cpp src/audio_decode.cpp src/platform.cpp src/folder_picker.cpp
                 src/render_mode.cpp src/process.cpp src/sysinfo.cpp)
  target_link_libraries(gh_tests PRIVATE raylib)
  if(WIN32)
    target_link_libraries(gh_tests PRIVATE shell32 ole32 advapi32)
  endif()
  if(APPLE)
    target_link_libraries(gh_tests PRIVATE "-framework Cocoa" "-framework IOKit" "-framework CoreFoundation")
  endif()
  target_compile_definitions(gh_tests PRIVATE GH_AUDIO_TESTS=1)
endif()

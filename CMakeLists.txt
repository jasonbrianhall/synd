cmake_minimum_required(VERSION 3.20)
if(APPLE)
  # one universal program for Apple silicon and Intel Macs (both are set before project() so they apply)
  set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "the architectures of the macOS build")
  set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "the oldest macOS the build runs on")
endif()
project(SyndicatePlus C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---- the generated code of the recompiled DOS programs (tools/gen_all.py) -------------------------------
set(GEN_MODULES synd syn_main intro ar_main syn_smain syn_gmain ar_smain ar_gmain sintro gintro drv_dg drv_fm)
set(GEN_SOURCES)
foreach(m ${GEN_MODULES})
  if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/gen/${m}/${m}.cmake")
    message(FATAL_ERROR "gen/${m} is missing: put the original Syndicate Plus files in _references/dos and run "
                        "'python3 tools/gen_all.py' (needs 'pip install capstone'); see README.md")
  endif()
  include(${CMAKE_CURRENT_SOURCE_DIR}/gen/${m}/${m}.cmake)
  string(TOUPPER ${m} M)
  list(APPEND GEN_SOURCES ${${M}_SOURCES})
endforeach()

# ---- the portable core: runtime + game + generated code -------------------------------------------------
add_library(syncore STATIC
  src/rt/core.c
  src/rt/dos.c
  src/rt/dpmi.c
  src/rt/devices.c
  src/rt/vga.c
  src/rt/strings.c
  src/rt/audio.c
  src/rt/sha1.c
  src/rt/vfs.c
  src/game/game.c
  ${GEN_SOURCES})
target_include_directories(syncore PUBLIC src)
option(RT_TRACE_CALLS "record guest function entries (debugging)" OFF)
if(RT_TRACE_CALLS)
  target_compile_definitions(syncore PUBLIC RT_TRACE_CALLS)
endif()
foreach(m ${GEN_MODULES})
  target_include_directories(syncore PRIVATE gen/${m})
endforeach()

if(MSVC)
  target_compile_options(syncore PRIVATE /W3 /utf-8 /wd4102 /wd4146 /wd4244 /wd4996)
  target_compile_definitions(syncore PRIVATE _CRT_SECURE_NO_WARNINGS)
  set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "/O2;/Ob2;/GF;/bigobj")
else()
  target_compile_options(syncore PRIVATE -Wall -Wno-comment -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable)
  # the generated code at -O2 as with MSVC (clang needs about 2.5 s and 180 MB per file there; -O3 brings
  # nothing measurable for this kind of code), with the warnings its mechanical style gives switched off
  set_source_files_properties(${GEN_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-O2;-Wno-unused-function;-Wno-parentheses-equality;-Wno-self-assign;-Wno-unused-value;-Wno-tautological-compare")
endif()

# ---- the portable presentation code the front ends share (no OS calls) -----------------------------------
add_library(synvideo STATIC src/video/crt.c)
target_include_directories(synvideo PUBLIC src)
if(MSVC)
  target_compile_options(synvideo PRIVATE /W3 /O2 /utf-8)
endif()

# ---- the original game files built into a private build ----------------------------------------------
# A private build carries the original game files (tools/pack_game.py) and needs no game folder; a public
# build is made with -DSYN_EMBED_GAME=OFF and asks the player for their own copy.
set(SYN_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/_references/dos" CACHE PATH "the original game files to build in")
if(EXISTS "${SYN_GAME_DIR}/SYND.EXE")
  set(_embed_default ON)
else()
  set(_embed_default OFF)
endif()
option(SYN_EMBED_GAME "build the original game files into the program (private builds only)" ${_embed_default})
if(SYN_EMBED_GAME)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  file(GLOB_RECURSE SYN_GAME_FILES CONFIGURE_DEPENDS "${SYN_GAME_DIR}/*")
  set(SYN_PAK "${CMAKE_CURRENT_BINARY_DIR}/syndgame.pak")
  add_custom_command(OUTPUT "${SYN_PAK}"
    COMMAND Python3::Interpreter "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_game.py" "${SYN_GAME_DIR}" "${SYN_PAK}"
    DEPENDS ${SYN_GAME_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/tools/pack_game.py"
    COMMENT "Packing the game files")
endif()

# ---- front ends -----------------------------------------------------------------------------------------
if(WIN32)
  # the game: Syndicate64.exe (both games in one program)
  add_executable(syndicate64 WIN32
    src/platform/win32/win32_main.c
    src/platform/win32/win32_sys.c
    res/syndicate64.rc)
  target_link_libraries(syndicate64 PRIVATE syncore synvideo winmm dwmapi uxtheme shell32 ole32 advapi32 user32 gdi32)
  target_compile_definitions(syndicate64 PRIVATE _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
  set_target_properties(syndicate64 PROPERTIES OUTPUT_NAME Syndicate64)
  if(MSVC)
    target_compile_options(syndicate64 PRIVATE /W3 /utf-8)
    target_link_options(syndicate64 PRIVATE /MANIFEST:NO)
  endif()

  # the private build: the pack as the resource GAMEPAK
  if(SYN_EMBED_GAME)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/gamepak.rc" "GAMEPAK RCDATA \"${SYN_PAK}\"\n")
    set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/gamepak.rc" PROPERTIES OBJECT_DEPENDS "${SYN_PAK}")
    target_sources(syndicate64 PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/gamepak.rc")
    message(STATUS "Syndicate64.exe carries the game files from ${SYN_GAME_DIR}")
  endif()

  add_executable(synd_headless
    src/platform/headless/headless_main.c
    src/platform/win32/win32_sys.c)
  target_link_libraries(synd_headless PRIVATE syncore synvideo winmm)
  target_compile_definitions(synd_headless PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(APPLE)
  enable_language(OBJC)
  set(SYN_VERSION "1.0.0")

  # the app icon: the badge of res/syndicate-icon.png on a tile of the macOS icon shape, at every size an .icns
  # holds (sips + iconutil)
  set(SYN_ICONSET "${CMAKE_CURRENT_BINARY_DIR}/Syndicate64.iconset")
  set(SYN_ICNS "${CMAKE_CURRENT_BINARY_DIR}/Syndicate64.icns")
  set(SYN_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/res/macos/syndicate-icon-macos.png") # tools/make_mac_icon.m
  set(_icon_cmds)
  foreach(sz 16 32 128 256 512)
    math(EXPR sz2 "${sz} * 2")
    list(APPEND _icon_cmds
      COMMAND sips -z ${sz} ${sz} "${SYN_ICON_PNG}" --out "${SYN_ICONSET}/icon_${sz}x${sz}.png" >/dev/null
      COMMAND sips -z ${sz2} ${sz2} "${SYN_ICON_PNG}" --out "${SYN_ICONSET}/icon_${sz}x${sz}@2x.png" >/dev/null)
  endforeach()
  add_custom_command(OUTPUT "${SYN_ICNS}"
    COMMAND ${CMAKE_COMMAND} -E rm -rf "${SYN_ICONSET}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SYN_ICONSET}"
    ${_icon_cmds}
    COMMAND iconutil -c icns "${SYN_ICONSET}" -o "${SYN_ICNS}"
    DEPENDS "${SYN_ICON_PNG}"
    COMMENT "Making the app icon")
  set_source_files_properties("${SYN_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)

  # the game: Syndicate64.app (both games in one program)
  add_executable(syndicate64 MACOSX_BUNDLE
    src/platform/macos/macos_main.m
    src/platform/posix/posix_sys.c
    "${SYN_ICNS}")
  set_target_properties(syndicate64 PROPERTIES
    OUTPUT_NAME Syndicate64
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/res/macos/Info.plist.in"
    MACOSX_BUNDLE_BUNDLE_NAME Syndicate64
    MACOSX_BUNDLE_GUI_IDENTIFIER com.buyukcaglar.syndicate64
    MACOSX_BUNDLE_BUNDLE_VERSION ${SYN_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${SYN_VERSION}
    MACOSX_BUNDLE_ICON_FILE Syndicate64.icns)
  target_compile_definitions(syndicate64 PRIVATE SYN_VERSION="${SYN_VERSION}")
  target_compile_options(syndicate64 PRIVATE -Wall -fobjc-arc)
  target_link_libraries(syndicate64 PRIVATE syncore synvideo
    "-framework Cocoa" "-framework AudioToolbox" "-framework CoreGraphics" "-framework QuartzCore")
  # the private build: the pack in Contents/Resources, mapped into memory at start
  if(SYN_EMBED_GAME)
    target_sources(syndicate64 PRIVATE "${SYN_PAK}")
    set_source_files_properties("${SYN_PAK}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    message(STATUS "Syndicate64.app carries the game files from ${SYN_GAME_DIR}")
  endif()
  # development checks of the app (scripted input inside its own process, snapshots): never in a release
  option(SYN_DEVTEST "build the app's development checks (src/platform/macos/macos_devtest.m)" OFF)
  if(SYN_DEVTEST)
    target_sources(syndicate64 PRIVATE src/platform/macos/macos_devtest.m)
    target_compile_definitions(syndicate64 PRIVATE SYN_DEVTEST)
  endif()
  # the signature: ad hoc by default; with a Developer ID (-DSYN_CODESIGN_IDENTITY="Developer ID Application: ...")
  # the hardened runtime and a secure timestamp, as notarization requires (tools/notarize_mac.sh)
  set(SYN_CODESIGN_IDENTITY "-" CACHE STRING "codesign identity for Syndicate64.app (- signs ad hoc)")
  if(SYN_CODESIGN_IDENTITY STREQUAL "-")
    set(_sign_opts --timestamp=none)
  else()
    set(_sign_opts --options runtime --timestamp)
  endif()
  add_custom_command(TARGET syndicate64 POST_BUILD
    COMMAND codesign --force --sign "${SYN_CODESIGN_IDENTITY}" ${_sign_opts} "$<TARGET_BUNDLE_DIR:syndicate64>"
    COMMENT "Signing Syndicate64.app (${SYN_CODESIGN_IDENTITY})"
    VERBATIM)

  add_executable(synd_headless
    src/platform/headless/headless_main.c
    src/platform/posix/posix_sys.c)
  target_link_libraries(synd_headless PRIVATE syncore synvideo)
  if(SYN_EMBED_GAME)
    # the tests' built-in-files run reads build/syndgame.pak
    add_custom_target(syndgame_pak ALL DEPENDS "${SYN_PAK}")
  endif()
endif()

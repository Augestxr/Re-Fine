cmake_minimum_required(VERSION 3.20)
project(ReFine
    VERSION 1.0.0
    LANGUAGES C CXX
    DESCRIPTION "Hatsune Miku: Project DIVA F (BLUS31319) - ps3recomp"
)

# ---------------------------------------------------------------------------
# Standards
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Prefer Ninja if available
if(NOT CMAKE_GENERATOR MATCHES "Ninja" AND NOT DEFINED ENV{CMAKE_GENERATOR})
    message(STATUS "Tip: use -G Ninja for faster builds")
endif()

# ---------------------------------------------------------------------------
# The two directories this project is built from
# ---------------------------------------------------------------------------
# PS3RECOMP_DIR is the toolkit checkout: include/, runtime/, libs/, tools/.
# RECOMP_DIR is where the lifter's output for THIS game lives. They are
# separate on purpose -- a port keeps its generated code in its own tree and
# points at whatever toolkit checkout it is building against:
#
#   cmake -B build -G Ninja \
#         -DPS3RECOMP_DIR=/path/to/ps3recomp \
#         -DRECOMP_DIR=/path/to/my_game/recompiled
#
if(NOT DEFINED PS3RECOMP_DIR)
    # Default: the pinned submodule
    get_filename_component(PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/external/ps3recomp" ABSOLUTE)
endif()
get_filename_component(PS3RECOMP_DIR "${PS3RECOMP_DIR}" ABSOLUTE)

set(RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/work/recompiled" CACHE PATH
    "Directory holding the lifter's output: ppu_recomp.h and the lifted sources")
get_filename_component(RECOMP_DIR "${RECOMP_DIR}" ABSOLUTE)

message(STATUS "ps3recomp SDK:   ${PS3RECOMP_DIR}")
message(STATUS "Lifted output:   ${RECOMP_DIR}")

if(NOT EXISTS "${RECOMP_DIR}/ppu_recomp.h")
    message(FATAL_ERROR
        "No ppu_recomp.h in ${RECOMP_DIR}.\n"
        "That header and the lifted sources beside it are what "
        "tools/ppu_lifter.py writes. Run the lifter, drop its output there, "
        "or point -DRECOMP_DIR= at wherever it already is.")
endif()

# The lifted translation units. A game's lift is hundreds of generated files,
# so they are globbed rather than listed; re-run CMake after a fresh lift.
file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS
    "${RECOMP_DIR}/*.c"
    "${RECOMP_DIR}/*.cpp"
)
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR
        "${RECOMP_DIR} has a ppu_recomp.h but no .c or .cpp beside it. The "
        "lifted functions and the function table live in those; without them "
        "there is nothing to run.")
endif()
list(LENGTH RECOMP_SOURCES RECOMP_SOURCE_COUNT)
message(STATUS "Lifted sources:  ${RECOMP_SOURCE_COUNT} file(s)")

# ---------------------------------------------------------------------------
# ps3recomp runtime library
# ---------------------------------------------------------------------------
# Built from source as a subdirectory rather than imported as a prebuilt
# archive. A static library carries none of its own link requirements, so an
# imported one arrives without SDL2, pthreads, the Metal frameworks or the
# Winsock/XInput imports -- the link then fails on every platform for a
# different reason. add_subdirectory hands all of that over with the target.
# FFmpeg (cellVdec, cellAdec): only its headers at build time; the DLLs load at
# run time and go next to the executable. tools/fetch_ffmpeg.ps1 brings both.
set(REFINE_FFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/work/ffmpeg" CACHE PATH "FFmpeg from tools/fetch_ffmpeg.ps1")
if(NOT EXISTS "${REFINE_FFMPEG_DIR}/include/libavcodec/avcodec.h")
    message(FATAL_ERROR "No FFmpeg in ${REFINE_FFMPEG_DIR}: run tools/fetch_ffmpeg.ps1")
endif()
set(PS3RECOMP_FFMPEG_RUNTIME ON CACHE BOOL "" FORCE)
set(PS3RECOMP_FFMPEG_INCLUDE_DIR "${REFINE_FFMPEG_DIR}/include" CACHE PATH "" FORCE)

add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp_sdk"
                 EXCLUDE_FROM_ALL)

# SDL3 (cellPad): every controller goes through it. tools/fetch_sdl3.ps1 brings the
# headers, the import library and the DLL that goes next to the executable.
set(REFINE_SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/work/sdl3" CACHE PATH "SDL3 from tools/fetch_sdl3.ps1")
if(NOT EXISTS "${REFINE_SDL3_DIR}/include/SDL3/SDL.h")
    message(FATAL_ERROR "No SDL3 in ${REFINE_SDL3_DIR}: run tools/fetch_sdl3.ps1")
endif()
target_include_directories(ps3recomp_runtime PRIVATE "${REFINE_SDL3_DIR}/include")
target_compile_definitions(ps3recomp_runtime PRIVATE PS3RECOMP_PAD_USE_SDL3)
target_link_libraries(ps3recomp_runtime PUBLIC "${REFINE_SDL3_DIR}/lib/SDL3.lib")

# N1 (docs/plan_nativo.md): the Vulkan backend (libs/video/rsx_vk_win.c, patch 0178), chosen at run time with
# REFINE_RENDERER=vulkan. Only the headers are needed to build: vulkan-1.dll is loaded at run time.
option(REFINE_VULKAN "Build the Vulkan backend (REFINE_RENDERER=vulkan)" ON)
if(REFINE_VULKAN)
    set(REFINE_VULKAN_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/work/vulkan-headers/include" CACHE PATH "Vulkan-Headers from tools/fetch_vulkan.ps1")
    if(NOT EXISTS "${REFINE_VULKAN_HEADERS}/vulkan/vulkan.h")
        message(FATAL_ERROR "No Vulkan headers in ${REFINE_VULKAN_HEADERS}: run tools/fetch_vulkan.ps1 (or -DREFINE_VULKAN=OFF)")
    endif()
    target_include_directories(ps3recomp_runtime PRIVATE "${REFINE_VULKAN_HEADERS}")
    # V2a: HLSL -> SPIR-V in process (dxcompiler.dll, loaded at run time; dxcapi.h from tools/fetch_vulkan.ps1)
    set(REFINE_DXC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/work/dxc" CACHE PATH "DXC from tools/fetch_vulkan.ps1")
    if(NOT EXISTS "${REFINE_DXC_DIR}/inc/dxcapi.h")
        message(FATAL_ERROR "No dxcapi.h in ${REFINE_DXC_DIR}/inc: run tools/fetch_vulkan.ps1")
    endif()
    target_include_directories(ps3recomp_runtime PRIVATE "${REFINE_DXC_DIR}/inc")
    target_sources(ps3recomp_runtime PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/external/ps3recomp/libs/video/rsx_vk_spirv.cpp")
    target_compile_definitions(ps3recomp_runtime PRIVATE REFINE_VULKAN)   # rsx_vk_win.c
    add_compile_definitions(REFINE_VULKAN)   # targets made from here on: src/main.cpp
endif()

# RmlUi 6.3 and FreeType 2.14.3 (6b+6c, the port's settings menu): sources from
# tools/fetch_rmlui.ps1, built here with the same compiler as the port, static, without
# samples. FreeType without zlib, bzip2, png, harfbuzz or brotli: the menu loads plain TTF.
set(REFINE_RMLUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/work/rmlui" CACHE PATH "RmlUi, FreeType and Inter from tools/fetch_rmlui.ps1")
if(NOT EXISTS "${REFINE_RMLUI_DIR}/RmlUi-6.3/CMakeLists.txt" OR NOT EXISTS "${REFINE_RMLUI_DIR}/freetype-2.14.3/CMakeLists.txt")
    message(FATAL_ERROR "No RmlUi or FreeType in ${REFINE_RMLUI_DIR}: run tools/fetch_rmlui.ps1")
endif()
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(ft_off FT_DISABLE_ZLIB FT_DISABLE_BZIP2 FT_DISABLE_PNG FT_DISABLE_HARFBUZZ FT_DISABLE_BROTLI)
    set(${ft_off} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory("${REFINE_RMLUI_DIR}/freetype-2.14.3" "${CMAKE_BINARY_DIR}/freetype" EXCLUDE_FROM_ALL)
# RmlUi only asks for the target Freetype::Freetype (CMake/Utilities.cmake in RmlUi).
add_library(Freetype::Freetype ALIAS freetype)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
add_subdirectory("${REFINE_RMLUI_DIR}/RmlUi-6.3" "${CMAKE_BINARY_DIR}/rmlui" EXCLUDE_FROM_ALL)
# RmlUi asks for -Wall, and clang-cl reads that as -Weverything: thousands of lines about
# code that is not ours. Off for it and for FreeType.
target_compile_options(rmlui_core PRIVATE -w)
target_compile_options(freetype PRIVATE -w)

# ---------------------------------------------------------------------------
# The HLE NID table
# ---------------------------------------------------------------------------
# Maps each firmware import NID onto the handler in the runtime that answers
# it. A port may keep a hand-curated one beside its lift; otherwise generate
# the canonical every-module table, which is what the toolkit's own boot
# harness uses.
if(EXISTS "${RECOMP_DIR}/ppu_hle_nids.cpp")
    set(HLE_NID_TABLE "${RECOMP_DIR}/ppu_hle_nids.cpp")
    message(STATUS "HLE NID table:   ${HLE_NID_TABLE}")
else()
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    set(HLE_NID_TABLE "${CMAKE_CURRENT_BINARY_DIR}/ppu_hle_nids.cpp")
    # The generator discovers handlers from library definitions. Rebuild when
    # an existing module changes, and reconfigure when a new module is added.
    file(GLOB_RECURSE HLE_NID_SOURCES CONFIGURE_DEPENDS
        "${PS3RECOMP_DIR}/libs/*.c")
    # A removed input cannot make the output older than a remaining dependency.
    # Track the list itself so deletions also remove stale handler references.
    set(HLE_NID_SOURCE_LIST "${CMAKE_CURRENT_BINARY_DIR}/hle_nid_sources.txt")
    file(GENERATE OUTPUT "${HLE_NID_SOURCE_LIST}" CONTENT "${HLE_NID_SOURCES}\n")
    add_custom_command(
        OUTPUT "${HLE_NID_TABLE}"
        COMMAND "${Python3_EXECUTABLE}"
                "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                --out "${HLE_NID_TABLE}" --all
        DEPENDS "${PS3RECOMP_DIR}/tools/gen_hle_nids.py"
                "${PS3RECOMP_DIR}/tools/nid_database.py"
                ${HLE_NID_SOURCES} "${HLE_NID_SOURCE_LIST}"
        COMMENT "Generating the HLE NID -> handler table")
    message(STATUS "HLE NID table:   generated (every module)")
endif()

# ---------------------------------------------------------------------------
# The game executable
# ---------------------------------------------------------------------------
# The PPU boot scaffold is compiled here rather than taken from the runtime
# library: every one of these files #includes the lifter's per-game
# ppu_recomp.h, so they can only be built against a particular lift.
set(PS3RECOMP_SCAFFOLD
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"
)

# ---------------------------------------------------------------------------
# Lifted SPU images (optional)
# ---------------------------------------------------------------------------
# A title that runs SPU work gets its images lifted into <SPU_DIR>/<image>/ by
# tools/spu_lifter.py, with a registration TU generated by
# tools/build_spu_workloads.py. cellSpurs dispatches them by content
# fingerprint, so the generated table is keyed to the images THIS build ships:
# a table generated from another release registers signatures that can never
# match, which is worse than shipping none. Both the table and the job-module
# registry self-register from constructors and are therefore optional -- a tree
# that has not lifted its SPU images yet configures and links without them.
set(SPU_DIR "${CMAKE_CURRENT_SOURCE_DIR}/work/spu" CACHE PATH
    "Directory holding the lifted SPU images (<SPU_DIR>/<image>/spu_recomp.c)")
option(PORT_ENABLE_SPU "Compile and register the title's lifted SPU images" ON)

set(PORT_SPU_INPUTS "" CACHE STRING
    "Input files to scan for all embedded SPU ELF programs (semicolon-separated decrypted ELF/PRX/data paths)")
set(SPU_SOURCES "")
if(PORT_ENABLE_SPU AND PORT_SPU_INPUTS)
    include("${PS3RECOMP_DIR}/cmake/PS3RecompSPU.cmake")
    ps3recomp_generate_spu("${CMAKE_CURRENT_BINARY_DIR}/generated_spu" ${PORT_SPU_INPUTS})
    list(APPEND SPU_SOURCES ${SPU_GENERATED_SOURCES}
         "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
elseif(PORT_ENABLE_SPU)
    file(GLOB SPU_LIFTED CONFIGURE_DEPENDS "${SPU_DIR}/*/spu_recomp.c")
    if(SPU_LIFTED)
        list(LENGTH SPU_LIFTED SPU_IMAGE_COUNT)
        message(STATUS "Lifted SPU:      ${SPU_IMAGE_COUNT} image(s)")
        # The musttail indirect-branch fast path. Excluded from the runtime
        # library on purpose (see the SDK root CMakeLists): it must be built
        # here, by clang, or the tail call degrades to a plain call and a guest
        # loop that branches indirectly walks the thread stack off its guard
        # page a few thousand iterations in.
        list(APPEND SPU_SOURCES ${SPU_LIFTED}
             "${PS3RECOMP_DIR}/runtime/spu/spu_dispatch_mt.c")
        # Generated beside the lift; each is present only once its generator
        # has been run, so each is included only if it is actually here.
        foreach(gen IN ITEMS spu_workloads.c jobmods_register.c taskset_policy_bytes.c)
            if(EXISTS "${SPU_DIR}/${gen}")
                list(APPEND SPU_SOURCES "${SPU_DIR}/${gen}")
            else()
                message(STATUS "  (no ${gen} -- not registered)")
            endif()
        endforeach()
    else()
        message(STATUS "Lifted SPU:      none found in ${SPU_DIR}")
    endif()
endif()

add_executable(${PROJECT_NAME}
    src/main.cpp
    src/rob_interp.cpp
    src/judge.cpp
    src/aet_time.cpp
    src/evt.cpp
    src/realtime.cpp
    src/stubs.cpp
    src/port_menu/port_menu.cpp
    src/mods.cpp
    src/port_menu/discord.cpp
    src/port_menu/extras.cpp
    src/crash_report.cpp
    src/port_menu/rml_d3d12.cpp
    src/port_menu/rml_vk.cpp
    ${PS3RECOMP_SCAFFOLD}
    ${HLE_NID_TABLE}
    ${RECOMP_SOURCES}
    ${SPU_SOURCES}
)

# The lifted SPU sources use bare #include "spu_context.h". Scope that include
# directory (and their flags) to those TUs alone: adding it to the whole target
# changes the compile command of every lifted PPU TU, and those are large enough
# that the needless rebuild is measured in tens of minutes.
#   SPU_USE_DISPATCH_MT -- route SPU_IB_DISPATCH through the musttail path.
#   -O2                 -- the target-wide -O1 exists for the huge PPU TUs; the
#                          SPU sources are small and decode-hot, so they get
#                          full optimization. The last -O on the line wins.
if(SPU_SOURCES)
    set_source_files_properties(${SPU_SOURCES} PROPERTIES
        INCLUDE_DIRECTORIES "${PS3RECOMP_DIR}/runtime/spu"
        COMPILE_DEFINITIONS SPU_USE_DISPATCH_MT)
    if(NOT MSVC OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        # REFINE_SPU_ARCH: the SPU helpers (spu_helpers.h) pick their SSE4.1 and AVX2
        # forms at compile time, and they are inlined here (docs/perf.md, D7).
        set(REFINE_SPU_ARCH "-msse4.1" CACHE STRING "Extra -m flags for the lifted SPU code (-msse4.1 or -mavx2)")
        set_source_files_properties(${SPU_SOURCES} PROPERTIES COMPILE_OPTIONS "-O2;${REFINE_SPU_ARCH}")
    endif()
endif()

# app.rc carries the icon, the version block and the manifest, and needs a
# resource compiler. Listing it as a source on a host that has none is not a
# link error, it is a "no rule to make" for a file nothing can consume.
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
    target_sources(${PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${RECOMP_DIR}"                            # the generated ppu_recomp.h
    "${PS3RECOMP_DIR}/include"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/memory"
    "${PS3RECOMP_DIR}/runtime/syscalls"
    "${PS3RECOMP_DIR}/runtime/platform"        # win32_compat.h and friends
    "${PS3RECOMP_DIR}/libs/video"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ps3recomp_runtime
    RmlUi::Core
)

# The menu's documents, language files and fonts go next to the executable, in ui/.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/src/port_menu/ui" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/ui"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${REFINE_RMLUI_DIR}/fonts" "$<TARGET_FILE_DIR:${PROJECT_NAME}>/ui/fonts")

if(REFINE_VULKAN)   # N1 V2a: the Vulkan backend compiles the guest's programs to SPIR-V with it
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${REFINE_DXC_DIR}/dxcompiler.dll" "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
endif()

# LoadLibrary finds these next to the executable. swresample is a dependency of
# avcodec; the rest of FFmpeg is not used.
foreach(dll avcodec-63 avutil-61 swresample-7 swscale-10)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${REFINE_FFMPEG_DIR}/bin/${dll}.dll" "$<TARGET_FILE_DIR:${PROJECT_NAME}>")
endforeach()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${REFINE_SDL3_DIR}/lib/SDL3.dll" "$<TARGET_FILE_DIR:${PROJECT_NAME}>")

# The runtime is C++, and on Apple also Objective-C. Letting CMake infer the
# link language from the generated C would leave the C++ standard library out.
set_target_properties(${PROJECT_NAME} PROPERTIES LINKER_LANGUAGE CXX)

# Windows system import libraries. dbghelp symbolizes crash-handler addresses
# against the PDB, user32 and gdi32 are what the backend's window and its GDI
# present are made of, and winmm is timeBeginPeriod. None of the four names
# anything a POSIX linker can find; the D3D12 backend's own imports come in
# through the #pragma comment(lib) in libs/video/rsx_d3d12_backend.c.
# Re:Fine: ReFine.map beside the exe, so a crash address (src/crash_report.cpp) resolves to a function
# with tools/crash_symbols.py; costs a few seconds of link, nothing at run time.
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE "/MAP")
endif()
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        dbghelp
        user32
        gdi32
        winmm
    )
endif()

# ---------------------------------------------------------------------------
# Compiler-specific flags
# ---------------------------------------------------------------------------
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(${PROJECT_NAME} PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fno-strict-aliasing
        # The lift spells PPC arithmetic as signed C arithmetic, which must wrap;
        # without this, clang exploits overflow UB (e.g. folds "x*x < 0" to false).
        -fwrapv
    )
elseif(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3 /wd4100 /wd4244 /wd4267
        /Zc:__cplusplus         # report correct __cplusplus value
        # ppu_recomp.h defines PPU_FENCE, the lowering of the guest's
        # sync/lwsync/eieio/isync, and in C that is <stdatomic.h>. The lifter
        # writes C++ by default, but --single-file writes ppu_recomp.c, and a
        # port that took it would not compile without this. The runtime's own
        # build already carries the same flag for the same reason.
        /experimental:c11atomics
    )
    target_compile_definitions(${PROJECT_NAME} PRIVATE
        _CRT_SECURE_NO_WARNINGS
    )
endif()

# The lifted translation units are machine-generated and can run to hundreds of
# thousands of lines each, so they get their own flags:
#
#   - warnings off. They are about generated code and nobody will act on them,
#     and at this volume they bury the diagnostics that matter.
#   - no floating-point contraction. arm64 fuses a multiply and an add by
#     default; a PPC fmadd is spelt as one where the lifter means one.
#   - /bigobj on MSVC. A single lifted file routinely passes the 65,536-section
#     limit of the default object format.
if(MSVC)
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "/w;/bigobj")
else()
    set_source_files_properties(${RECOMP_SOURCES} PROPERTIES
        COMPILE_OPTIONS "-w;-ffp-contract=off")
endif()

# REFINE_INLINE_MEM: compile the lifted guest loads and stores inline (ppu_recomp.h).
# Off for development: ppu_recomp_000.cpp takes 975 s with them and 219 s without
# (docs/perf.md). On only to measure performance, from milestone 5 on, against
# the same build with it off (docs/decisions.md).
option(REFINE_INLINE_MEM "Inline the lifted guest memory accesses (slow to compile)" OFF)
if(NOT REFINE_INLINE_MEM)
    set_property(SOURCE ${RECOMP_SOURCES} APPEND PROPERTY COMPILE_DEFINITIONS
        PPU_NO_INLINE_LOADS PPU_NO_INLINE_STORES)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS ${PROJECT_NAME} RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# ReFineSetup: the installer (hito 8). Same RmlUi renderer and theme as the start screen; it
# carries nothing of the game. Output: build/setup/ReFineSetup.exe + build/setup/ui/.
# ---------------------------------------------------------------------------
if(WIN32 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tools/installer/setup/setup.cpp")   # the installer's payload has no setup sources
    add_executable(ReFineSetup WIN32
        tools/installer/setup/setup.cpp
        src/port_menu/rml_d3d12.cpp)
    set_target_properties(ReFineSetup PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/setup" LINKER_LANGUAGE CXX)
    target_link_libraries(ReFineSetup PRIVATE RmlUi::Core d3d12 dxgi d3dcompiler ole32 shell32 shlwapi uuid user32 gdi32 advapi32)
    add_custom_command(TARGET ReFineSetup POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/tools/installer/setup/ui" "$<TARGET_FILE_DIR:ReFineSetup>/ui"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${REFINE_RMLUI_DIR}/fonts" "$<TARGET_FILE_DIR:ReFineSetup>/ui/fonts")
endif()

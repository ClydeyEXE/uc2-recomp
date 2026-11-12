cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Unreal Championship 2 - Static Recompilation
#
# Copy this template to your game project root and customize:
#   1. Change the project name below
#   2. Set XBOXRECOMP_DIR to where you cloned xboxrecomp
#   3. Adjust source file lists as you add game-specific code
#   4. Add any game-specific compile definitions or link libraries
# ============================================================================

project(ucc2_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# ── xboxrecomp toolkit ──────────────────────────────────────────
#
# Option A: xboxrecomp as a subdirectory (simplest)
#   Place xboxrecomp/ next to your game project and use a relative path.
#
# Option B: find_package (if you install xboxrecomp system-wide)
#   find_package(xboxrecomp REQUIRED)
#
# Adjust this path to point to your xboxrecomp clone:
set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# Add xboxrecomp as a subdirectory -- this builds all 6 runtime libraries
# and provides the 'xboxrecomp' umbrella target to link against.
add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── Recompiled game executable ──────────────────────────────────

# Collect generated recompiled code.
# The recomp pipeline outputs files into src/recomp/gen/.
# With --split 1000, you get gen_0000.c, gen_0001.c, etc.
# CONFIGURE_DEPENDS so re-running the recompiler picks up added/removed files
# without a manual re-configure. A plain GLOB is evaluated once at configure
# time, so a newly generated file (an extra chunk, recomp_stubs_unresolved.c)
# is silently left out of the build and shows up as unresolved externals.
file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "src/recomp/gen/*.c")

# Core game sources
set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/gpu_d3d11.c
    # Add game-specific sources here, e.g.:
    # src/asset_loader.c
    # src/custom_renderer.c
)

# The recomp pipeline also generates a dispatch header and function declarations.
# These are included by the generated .c files:
#   src/recomp/gen/recomp_funcs.h    - extern declarations for all recompiled functions
#   src/recomp/recomp_dispatch.h     - binary search dispatch table
#   src/recomp/recomp_types.h        - register model, ICALL macros, MEM helpers

add_executable(${PROJECT_NAME} WIN32
    ${GAME_SOURCES}
    ${RECOMP_GEN_SOURCES}
)

# The version the log and the settings panel show (tools/version.cmake), worked out on every build.
add_custom_target(version_header
    COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/version/version.h
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/version.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/version/version.h)
add_dependencies(${PROJECT_NAME} version_header)

# Link against the xboxrecomp umbrella target (all 6 runtime libraries)
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Include paths for generated recomp headers
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/recomp
    ${CMAKE_BINARY_DIR}/version
    ${XBOXRECOMP_DIR}/include
    ${XBOXRECOMP_DIR}/src
)

# Windows SDK libraries needed at link time
target_link_libraries(${PROJECT_NAME} PRIVATE
    d3d11
    d3dcompiler
    dxgi
    dxguid
    $<IF:$<BOOL:${MINGW}>,xinput1_4,xinput>   # see src/input/CMakeLists.txt
    winmm
    # Lets the fault handler resolve the host RIP back to the generated
    # function's symbol, i.e. to the guest function that faulted.
    dbghelp
)

# MSVC-specific: large address aware, increase section limit for big generated files
if(MSVC)
    target_link_options(${PROJECT_NAME} PRIVATE
        /LARGEADDRESSAWARE
        # A PDB with every sub_XXXXXXXX public, so tools/prof.py and the fault
        # handler can name guest functions. /OPT keeps the Release layout.
        /DEBUG:FULL /OPT:REF /OPT:ICF
    )
    # Names for the runtime's statics in profiles.
    target_compile_options(xbox_kernel PRIVATE /Zi)
    target_compile_options(xbox_video PRIVATE /Zi)
    # And for this project's own sources and the generated code: its PDB then
    # names every function, not only the publics. Each generated file has its
    # own copy of recomp_types.h's static helpers (recomp_fp_round24 and the
    # rest), and without names a profile put those under whatever public came
    # before them ("sscanf"). Code generation is the same either way.
    target_compile_options(${PROJECT_NAME} PRIVATE /Zi)
    # The generated code files can be very large. Increase object file section limit.
    target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
    # MSVC stops optimizing a function past a size limit (warning C4883, off at
    # the default level) and compiles it as if with /Od: every guest register
    # and flag through memory, the TLS block looked up again on every access.
    # 34 generated functions were past it, among them several of the game
    # thread's hottest (sub_000FF840, sub_0014B0C0, sub_00188300), and with 7
    # bots lifting the limit took a match from 113-121 to 144 fps. The two
    # largest (sub_001BBD30, sub_001E6070, over 10,000 instructions) would then
    # take an hour to compile, so tools.recomp leaves those unoptimized with a
    # pragma and says so in recomp_funcs.h. Code generated before that has no
    # pragma, so the limit stays until it is regenerated.
    set(_recomp_funcs_h "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen/recomp_funcs.h")
    set(_recomp_size_limit "")
    if(EXISTS "${_recomp_funcs_h}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_recomp_funcs_h}")
        file(STRINGS "${_recomp_funcs_h}" _recomp_size_limit
             REGEX "^#define RECOMP_UNOPTIMIZED_INSNS" LIMIT_COUNT 1)
    endif()
    if(_recomp_size_limit)
        target_compile_options(${PROJECT_NAME} PRIVATE /d2OptimizeHugeFunctions)
    else()
        message(STATUS "Generated code predates RECOMP_UNOPTIMIZED_INSNS: its largest functions stay "
                       "unoptimized. Regenerate (tools/setup.ps1 -Regenerate) for about 20% more fps.")
    endif()
endif()

# ── Game-specific additions ─────────────────────────────────────
#
# Add any game-specific libraries, loaders, or custom renderers below.
# Examples:
#
# # Custom texture loader for your game's format
# target_sources(${PROJECT_NAME} PRIVATE src/texture_loader.c)
#
# # If your game uses RenderWare, you may need a RW bridge layer
# target_sources(${PROJECT_NAME} PRIVATE src/rw_bridge.c)
#
# # Additional compile definitions for debugging
# target_compile_definitions(${PROJECT_NAME} PRIVATE
#     ENABLE_ICALL_TRACE=1
#     VERBOSE_KERNEL_LOG=1
# )

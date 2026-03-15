cmake_minimum_required(VERSION 3.10)

# VitaSDK toolchain — must be set before project()
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake"
        CACHE PATH "VitaSDK toolchain file")
  else()
    message(FATAL_ERROR "Set VITASDK env var or pass -DCMAKE_TOOLCHAIN_FILE=...")
  endif()
endif()

project(pokeemerald C)

include("${VITASDK}/share/vita.cmake" REQUIRED)

# ----------------------------------------------------------------------------
# App metadata
# ----------------------------------------------------------------------------
set(VITA_APP_NAME  "pokeemerald")
set(VITA_TITLEID   "PKEMRLD00")
set(VITA_VERSION   "01.00")

# ----------------------------------------------------------------------------
# Asset generation: produce .lz / .gbapal / .4bpp / font files from PNG/pal
# sources using gbagfx. Run once at configure time; re-run cmake to refresh.
# ----------------------------------------------------------------------------
message(STATUS "Generating graphics assets (gbagfx)...")
execute_process(
    COMMAND bash "${CMAKE_SOURCE_DIR}/tools/generate_assets.sh" "${CMAKE_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _gfx_result
    OUTPUT_VARIABLE _gfx_out
    ERROR_VARIABLE  _gfx_err
)
if(_gfx_result)
    message(WARNING "generate_assets.sh failed: ${_gfx_err}")
else()
    message(STATUS "Assets: ${_gfx_out}")
endif()

# ----------------------------------------------------------------------------
# Preproc: expand INCBIN_U* macros in .c files to C data arrays.
# Produces build/preproc_src/<name>.preproc.c for each INCBIN-using source.
# ----------------------------------------------------------------------------
set(PREPROC_DIR "${CMAKE_CURRENT_BINARY_DIR}/preproc_src")
message(STATUS "Running preproc on INCBIN source files...")
execute_process(
    COMMAND bash "${CMAKE_SOURCE_DIR}/tools/run_preproc.sh"
                 "${CMAKE_SOURCE_DIR}"
                 "${PREPROC_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    RESULT_VARIABLE _pp_result
    OUTPUT_VARIABLE _pp_out
    ERROR_VARIABLE  _pp_err
)
if(_pp_result)
    message(WARNING "run_preproc.sh failed: ${_pp_err}")
else()
    message(STATUS "Preproc: ${_pp_out}")
endif()

# ----------------------------------------------------------------------------
# Source files
# ----------------------------------------------------------------------------

# All C files under src/ — we'll remove the ones we don't want below
file(GLOB_RECURSE SRC_GAME
    "${CMAKE_SOURCE_DIR}/src/*.c"
)

# Remove the Wii/SDL2 platform layer (replaced by vita.c)
list(FILTER SRC_GAME EXCLUDE REGEX ".*/platform/sdl2\\.c$")
# Exclude GBA-only assembly stubs (rom_header, crt0, Wii multiboot)
list(FILTER SRC_GAME EXCLUDE REGEX ".*/src/(rom_header|crt0|libgcnmultiboot|m4a_1)\\.s$")

# Replace INCBIN-containing .c files with their preproc'd equivalents
# (preproc expanded INCBIN_U* to C data arrays so the compiler gets real data)
file(GLOB PREPROC_SRCS "${PREPROC_DIR}/*.preproc.c")
if(PREPROC_SRCS)
    foreach(pp_src ${PREPROC_SRCS})
        get_filename_component(pp_name ${pp_src} NAME_WE)  # e.g. "graphics"
        string(REGEX REPLACE "\\.preproc$" "" orig_name "${pp_name}")  # strip .preproc
        # Remove the original src/*.c from SRC_GAME
        list(FILTER SRC_GAME EXCLUDE REGEX ".*/${orig_name}\\.c$")
        # Add the preproc'd version (no -include flags needed; globals already expanded)
        list(APPEND SRC_GAME ${pp_src})
    endforeach()
    message(STATUS "Using ${${PREPROC_SRCS}} preproc'd source files")
else()
    message(WARNING "No preproc'd files found in ${PREPROC_DIR} — INCBIN data will be zero")
endif()

# Add vita platform layer + CGB audio + stubs
list(APPEND SRC_GAME
    "${CMAKE_SOURCE_DIR}/src/platform/vita/vita.c"
    "${CMAKE_SOURCE_DIR}/src/platform/vita/vita_stubs.c"
    "${CMAKE_SOURCE_DIR}/src/platform/cgb_audio.c"
)

# Graphics library
file(GLOB SRC_GFLIB "${CMAKE_SOURCE_DIR}/gflib/*.c")

# Data assembly files (battle scripts, maps, event scripts, text, sound table)
# Custom pipeline: gcc -E → sed (fix :: labels) → arm-vita-eabi-as
# This avoids C pre-includes leaking in and handles the :: global-label syntax.
file(GLOB DATA_ASM_SRCS "${CMAKE_SOURCE_DIR}/data/*.s")

set(DATA_ASM_OBJS "")
foreach(asm_src ${DATA_ASM_SRCS})
    get_filename_component(asm_name ${asm_src} NAME_WE)
    set(asm_obj "${CMAKE_CURRENT_BINARY_DIR}/data_asm/${asm_name}.o")
    add_custom_command(
        OUTPUT  ${asm_obj}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/data_asm"
        COMMAND "${CMAKE_SOURCE_DIR}/tools/vita_asm.sh"
                "${asm_src}"
                "${asm_obj}"
                "${CMAKE_SOURCE_DIR}"
                "${CMAKE_SOURCE_DIR}/include"
        DEPENDS ${asm_src} "${CMAKE_SOURCE_DIR}/tools/vita_asm.sh"
        COMMENT "Assembling ${asm_name}.s"
    )
    list(APPEND DATA_ASM_OBJS ${asm_obj})
endforeach()

set(ALL_SOURCES ${SRC_GAME} ${SRC_GFLIB} ${DATA_ASM_OBJS})

# ----------------------------------------------------------------------------
# Executable
# ----------------------------------------------------------------------------
add_executable(${PROJECT_NAME}.elf ${ALL_SOURCES})

target_include_directories(${PROJECT_NAME}.elf PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/gflib
    ${CMAKE_CURRENT_BINARY_DIR}/preproc_headers  # preproc'd .h data files (MUST be before src/)
    ${CMAKE_SOURCE_DIR}/src   # fallback for non-preproc'd data headers
)

target_compile_options(${PROJECT_NAME}.elf PRIVATE
    -O2
    -Wall
    -Wno-pointer-to-int-cast
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -fno-strict-aliasing
    -fsigned-char
    # Suppress warnings from original GBA code
    -Wno-implicit-function-declaration
    -Wno-int-conversion
    -Wno-pointer-sign
    # Pre-include types first so u8/u16/u32 are defined before global.h pulls in strings.h via <string.h>
    "SHELL:-include ${CMAKE_SOURCE_DIR}/include/gba/types.h"
    "SHELL:-include ${CMAKE_SOURCE_DIR}/include/global.h"
)

target_compile_definitions(${PROJECT_NAME}.elf PRIVATE
    PORTABLE=1
    NONMATCHING=1
    UBFIX=1
    MODERN=0
)

target_link_libraries(${PROJECT_NAME}.elf
    vita2d
    SceAppMgr_stub
    SceSysmodule_stub
    SceCtrl_stub
    SceDisplay_stub
    SceGxm_stub
    SceCommonDialog_stub
    SceIofilemgr_stub
    m
    c
)

# ----------------------------------------------------------------------------
# Vita packaging
# ----------------------------------------------------------------------------
vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME}.elf)

vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
    VERSION ${VITA_VERSION}
    NAME    ${VITA_APP_NAME}
)

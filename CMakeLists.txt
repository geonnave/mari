# mari — opt-in CMake build.
#
# Additive: SES (.emProject) projects continue to work unchanged. Only
# consumers that opt into CMake see the target below.
#
# Usage from a downstream CMake project:
#
#   set(MARI /path/to/mari)
#   add_subdirectory(${MARI} mari-build)
#
#   add_executable(my_fw main.c ...)
#   target_link_libraries(my_fw PRIVATE mari_libs)
#   target_compile_definitions(my_fw PRIVATE
#       NRF5340_XXAA NRF_APPLICATION
#       # add USE_SWARMIT if linking against the swarmit bootloader,
#       # which provides swarmit_init_rng/swarmit_read_rng (see
#       # drv/mr_rng/mr_rng_nrf5340_app.c). Without USE_SWARMIT the
#       # default path uses NRF_RNG directly and standalone-links.
#   )
#
# Consumer responsibilities (not enforced here):
#   * Cross-compiling toolchain (gcc-arm-none-eabi recommended).
#   * Nordic MDK device headers (nrf.h, ...) and ARM CMSIS-Core
#     on the include path. Easy path: FetchContent
#     `NordicSemiconductor/nrfx` and `ARM-software/CMSIS_5`.
#   * Board / chip defines (see usage example).
#   * Startup code, linker script, Reset_Handler.
#   * dotbot-libs's BSP if the consumer also uses db_*. mari does not
#     itself link against dotbot-libs — its mr_* drivers are
#     deliberate, prefix-isolated re-implementations of the same
#     peripheral surface (per workspace AGENTS.md "Coupling violations"
#     #1: mari's mr_* BSP files are intended duplicates of
#     dotbot-libs/bsp/nrf/* re-prefixed so binaries can link mari
#     alongside dotbot-libs without symbol collisions).
#
# Why INTERFACE + target_sources (not STATIC):
#   Consumers compile under their own defines (NRF_APPLICATION vs
#   NRF_NETWORK, USE_SWARMIT, ...) which flip the per-chip dispatcher
#   pattern in mr_radio.c / mr_rng.c. INTERFACE lets each consumer
#   compile the same sources under its own define set.
#
# Scope of this first cut: the nRF5340-app-core source set known to
# build under gcc-arm-none-eabi when linked into a downstream
# application (validated standalone via swarmit consumer; mari does
# not produce a runnable binary on its own).

cmake_minimum_required(VERSION 3.20)

project(mari LANGUAGES C ASM)

add_library(mari_libs INTERFACE)

target_sources(mari_libs INTERFACE
    # MAC / link-layer core.
    # NOTE: all_schedules.c and association.c are NOT listed — scheduler.c
    # #include's them directly as one compile unit (see scheduler.c:22-23).
    # Adding them here would compile them twice and produce duplicate symbols.
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/mari/bloom.c
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/mari/mac.c
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/mari/mari.c
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/mari/packet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/mari/queue.c
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/mari/scan.c
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/mari/scheduler.c

    # Drivers (BSP-like). Each *.c is a dispatcher that #include's the
    # per-target variant (e.g. mr_rng.c includes mr_rng_nrf5340_app.c
    # or mr_rng_default.c based on NRF5340_XXAA + NRF_APPLICATION).
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/drv/mr_clock/mr_clock.c
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/drv/mr_gpio/mr_gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/drv/mr_radio/mr_radio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/drv/mr_rng/mr_rng.c
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/drv/mr_timer_hf/mr_timer_hf.c
)

target_include_directories(mari_libs INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/drv
    ${CMAKE_CURRENT_SOURCE_DIR}/firmware/mari
)

# gcc-arm-none-eabi 14+ promoted several -W diagnostics from warning to
# error. Mari has a few legacy patterns that armclang and older gcc
# accepted silently; demoting them back to warnings lets the existing
# source compile under modern toolchains. Fixing the source is
# deliberately out of scope for the CMake migration:
#
#   -Wint-conversion / -Wpointer-compare:
#     `cell->assigned_node_id = NULL` where the field is uint64_t (uses
#     0/NULL interchangeably as the unassigned sentinel) —
#     firmware/mari/bloom.c:87, scheduler.c:115/166, association.c:389.
#
#   -Wimplicit-function-declaration:
#     firmware/drv/mr_rng/mr_rng_nrf5340_app.c calls
#     db_configure_ram_non_secure / db_ipc_network_call /
#     release_network_core without `#include "tz.h"` or `#include
#     "ipc.h"`. SES projects added the dotbot-libs headers globally;
#     gcc 14 demands explicit includes. Symbols resolve at link time
#     when consumers also link dotbot_libs.
#
# Listed at INTERFACE level so the demotion applies whenever a
# consumer compiles mari sources. It does not relax the consumer's
# own translation units beyond these same legacy patterns.
target_compile_options(mari_libs INTERFACE
    -Wno-error=int-conversion
    -Wno-error=pointer-compare
    -Wno-error=implicit-function-declaration
)


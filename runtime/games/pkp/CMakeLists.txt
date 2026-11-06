# SPDX-License-Identifier: GPL-3.0-or-later
# The Pokémon Pokopia game module (docs/GAME_BOUNDARY.md, pkp_game.h). Libraries, each a superset of the one before:
#   nxrt_game_pkp_data   identity, branding (pure data; the launcher links it)
#   nxrt_game_pkp_core   + title facts, test probes (links nxrt)
#   nxrt_game_pkp        + play-mode hooks (links nxrt_gfx; only with NXRT_BUILD_GFX). pkp.exe links this one.
set(_pkp "${CMAKE_CURRENT_SOURCE_DIR}")
set(_rt "${PROJECT_SOURCE_DIR}")
# Identity and file names come from the game manifest (recomp/games/pkp/game.toml) through a generated header (pkp_data.cpp).
get_filename_component(_pkp_recomp "${_rt}/../recomp" ABSOLUTE)
include("${_pkp_recomp}/cmake/GameManifest.cmake")
game_manifest_load("${_pkp_recomp}/games/pkp/game.toml")
set(_pkp_gen "${CMAKE_CURRENT_BINARY_DIR}/gen")
game_manifest_header("${_pkp_gen}/game_manifest.h")
set(NXRT_GAME_PKP_INCLUDES "${_rt}/include" "${_rt}/games" "${_pkp}" "${_rt}/src/gfx" "${_rt}/src/gfx/play" "${_rt}/src/testtools")

add_library(nxrt_game_pkp_data STATIC "${_pkp}/pkp_data.cpp")
target_include_directories(nxrt_game_pkp_data PUBLIC ${NXRT_GAME_PKP_INCLUDES})
target_include_directories(nxrt_game_pkp_data PRIVATE "${_pkp_gen}")
target_compile_features(nxrt_game_pkp_data PUBLIC cxx_std_20)

add_library(nxrt_game_pkp_core STATIC "${_pkp}/pkp_core.cpp" "${_pkp}/pkp_tt_probe.cpp")
target_link_libraries(nxrt_game_pkp_core PUBLIC nxrt_game_pkp_data nxrt)

if(TARGET nxrt_gfx)
    add_library(nxrt_game_pkp STATIC "${_pkp}/pkp_game.cpp" "${_pkp}/pkp_play.cpp" "${_pkp}/pkp_kbm.cpp" "${_pkp}/pkp_menu.cpp" "${_pkp}/pkp_movie_test.cpp" "${_pkp}/pkp_quality.cpp" "${_pkp}/pkp_taa.cpp")
    target_link_libraries(nxrt_game_pkp PUBLIC nxrt_game_pkp_core nxrt_gfx)
endif()

# nxe_dash - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(nxe_dash LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(NXEDASH_SOURCES
    src/main.cpp
    src/install_paths.cpp
    src/kernel_stubs.cpp
    src/crash_report.cpp
    src/storage_device.cpp
    src/kernel_io.cpp
    src/video_mode.cpp
    src/xconfig.cpp
    src/network.cpp
    src/host_ip.cpp
    src/media_loader.cpp
    src/video_library.cpp
    src/xhttp.cpp
    src/xlivebase.cpp
    src/boot_video.cpp
    src/signin_trace.cpp
    src/user_profile.cpp
    src/profile_settings.cpp
    src/account_decrypt.cpp
    src/gamer_tile.cpp
    src/gpd_images.cpp
    src/title_library.cpp
    src/achievement_list.cpp
    src/shared_resources.cpp
    src/game_launch.cpp
    src/discord_presence.cpp
    src/cover_art.cpp
    src/played_titles.cpp
    src/content_enum.cpp
    src/content_resolve.cpp
    src/profile_read.cpp
    src/content_metadata.cpp
    src/content_enum_simple.cpp
    src/title_identity.cpp
    src/disc_state.cpp
    src/profile_ui.cpp
    src/live_account.cpp
    src/live_signin.cpp
    src/live_logon.cpp
    src/live_dialog.cpp
    src/date_format.cpp
    src/channel_trace.cpp
    src/eco_trace.cpp
    src/eco_locale.cpp
    src/channel_slots.cpp
    src/theme_trace.cpp
    src/guide_bridge.cpp
    src/avatar_bridge.cpp
    src/avatar_editor_launch.cpp
)

if(WIN32)
    # The .rc carries the executable icon; only meaningful on Windows.
    add_executable(nxe_dash WIN32 ${NXEDASH_SOURCES} src/res/nxe_dash.rc)
else()
    add_executable(nxe_dash ${NXEDASH_SOURCES} src/res/nxe_dash.rc)
endif()

rexglue_setup_target(nxe_dash)

# Overrides compiled into the recompiled Guide module itself.
#
# They must be inside nxe_dash_xam, not the executable: recompiled code calls its
# internal functions directly, so only a strong definition linked into the same
# binary can displace the weak alias. See src/guide_xam_overrides.cpp.
if(TARGET nxe_dash_xam)
    target_sources(nxe_dash_xam PRIVATE src/guide_xam_overrides.cpp)
endif()

# A .map beside each binary, so src/crash_report.cpp's "module+RVA" frames can
# be turned back into guest sub_ symbols. Without it a fault inside recompiled
# XAM names a module and an offset and nothing more.
# Media Foundation, for the boot animation (src/boot_video.cpp). mfplay is the
# HWND-bound player; mfplat provides MFStartup.
if(WIN32)
    target_link_libraries(nxe_dash PRIVATE mfplay mfplat)
endif()

# The executable is NXE.exe, matching the application name the SDK reports and
# the config file it reads (NXE.toml). The CMake target keeps its own name.
set_target_properties(nxe_dash PROPERTIES OUTPUT_NAME "NXE")

target_link_options(nxe_dash PRIVATE "LINKER:/MAP:nxe_dash.map")
if(TARGET nxe_dash_xam)
    target_link_options(nxe_dash_xam PRIVATE "LINKER:/MAP:nxe_dash_xam.map")
endif()

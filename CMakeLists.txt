cmake_minimum_required(VERSION 3.22)
set(PROJECT_VER "0.1.0")
# Private machine-local defaults (Wi-Fi, ONO server URL, token...) live in
# sdkconfig.defaults.local, which is git-ignored. Use it when present.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
    if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
        list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.local")
    endif()
endif()
# ESP-Hosted is pinned to 3.0.9 to match the bundled C6 firmware (same as the
# ADS-B display). Skip the "new versions available" check, which trips over the
# registry metadata of newer ESP-Hosted releases.
set(ENV{IDF_COMPONENT_CHECK_NEW_VERSION} 0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ono_display)

cmake_minimum_required(VERSION 3.16)

# Leftover firmware/sdkconfig (gitignored) keeps old Kconfig values
# across PRs and ignores sdkconfig.defaults. Drop it when size flags
# do not match: Debug (-Og) overflowed 0x1C0000; 3484b00 -Os still
# overflowed by 25648 B while MAXIMUM DEBUG strings were compiled in.
set(_sb_sdkconfig "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
if(EXISTS "${_sb_sdkconfig}")
    file(READ "${_sb_sdkconfig}" _sb_sdk_txt)
    set(_sb_stale FALSE)
    if(_sb_sdk_txt MATCHES "CONFIG_COMPILER_OPTIMIZATION_DEBUG=y")
        set(_sb_stale TRUE)
    endif()
    if(NOT _sb_sdk_txt MATCHES "CONFIG_COMPILER_OPTIMIZATION_SIZE=y")
        set(_sb_stale TRUE)
    endif()
    if(_sb_sdk_txt MATCHES "CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y")
        set(_sb_stale TRUE)
    endif()
    if(NOT _sb_sdk_txt MATCHES "CONFIG_LOG_MAXIMUM_LEVEL_INFO=y")
        set(_sb_stale TRUE)
    endif()
    if(_sb_stale)
        message(STATUS "searaboom: dropping leftover sdkconfig so sdkconfig.defaults size flags apply")
        file(REMOVE "${_sb_sdkconfig}")
        if(EXISTS "${_sb_sdkconfig}.old")
            file(REMOVE "${_sb_sdkconfig}.old")
        endif()
    endif()
endif()

file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/VERSION")

include($ENV{ADF_PATH}/CMakeLists.txt)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# ADF stack_in_ext uses xTaskCreateRestrictedPinnedToCore (weak stub unless
# $ADF_PATH/idf_patches/idf_v5.3_freertos.patch is applied). Without it,
# http/aac/clip tasks fail to start and the box is silent.
file(READ "$ENV{IDF_PATH}/components/freertos/esp_additions/include/freertos/idf_additions.h" _idf_additions)
if(NOT _idf_additions MATCHES "xTaskCreateRestrictedPinnedToCore")
    message(FATAL_ERROR
        "IDF is missing xTaskCreateRestrictedPinnedToCore. From IDF_PATH run:\n"
        "  git apply $ENV{ADF_PATH}/idf_patches/idf_v5.3_freertos.patch")
endif()

project(searaboom)

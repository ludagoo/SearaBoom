cmake_minimum_required(VERSION 3.16)

# Leftover firmware/sdkconfig (gitignored) keeps Debug (-Og) across PRs
# and ignores sdkconfig.defaults. Debug overflowed the 0x1C0000 OTA slot
# after tune_fill.aac. Drop it so a normal idf.py build takes SIZE=y.
set(_sb_sdkconfig "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
if(EXISTS "${_sb_sdkconfig}")
    file(STRINGS "${_sb_sdkconfig}" _sb_opt REGEX "^CONFIG_COMPILER_OPTIMIZATION_DEBUG=y")
    if(_sb_opt)
        message(STATUS "searaboom: dropping leftover Debug sdkconfig (use sdkconfig.defaults size-opt)")
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

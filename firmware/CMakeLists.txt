cmake_minimum_required(VERSION 3.16)

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

cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/VERSION")

# Link-only: symlink the existing config key. Does not pass --generate (no mint).
execute_process(
    COMMAND "${CMAKE_CURRENT_LIST_DIR}/../scripts/ensure_signing_key.sh"
    RESULT_VARIABLE _sb_key_rc
    OUTPUT_VARIABLE _sb_key_out
    ERROR_VARIABLE _sb_key_err
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _sb_key_rc EQUAL 0)
    message(FATAL_ERROR
        "secure boot signing key (link-only, no mint):\n${_sb_key_err}\n"
        "If you intend to create a new key once:\n"
        "  ${CMAKE_CURRENT_LIST_DIR}/../scripts/ensure_signing_key.sh --generate\n"
        "See docs/SIGNED_FIRMWARE.md")
endif()
message(STATUS "Firmware signing key: ${_sb_key_out}")

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

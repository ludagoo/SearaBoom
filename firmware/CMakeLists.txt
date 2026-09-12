cmake_minimum_required(VERSION 3.16)

file(READ "${CMAKE_CURRENT_LIST_DIR}/VERSION" PROJECT_VER)
string(STRIP "${PROJECT_VER}" PROJECT_VER)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/VERSION")

# Signed OTA needs firmware/secure_boot_signing_key.pem at configure.
# Do not mint a trust anchor here — cmake must not call ensure_signing_key.sh.
set(_sb_key "${CMAKE_CURRENT_LIST_DIR}/secure_boot_signing_key.pem")
if(NOT EXISTS "${_sb_key}")
    message(FATAL_ERROR
        "Missing ${_sb_key}.\n"
        "A build must not generate the fleet signing key. Once, if you intend to keep it:\n"
        "  ${CMAKE_CURRENT_LIST_DIR}/../scripts/ensure_signing_key.sh --generate\n"
        "If ~/.config/searaboom/secure_boot_signing_key.pem already exists, run the script\n"
        "without --generate to symlink the firmware path. See docs/SIGNED_FIRMWARE.md")
endif()
message(STATUS "Firmware signing key: ${_sb_key}")

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

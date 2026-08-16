# Edit following two lines to set component requirements (see docs)
set(COMPONENT_REQUIRES)
set(COMPONENT_PRIV_REQUIRES audio_sal audio_hal esp_dispatcher esp_peripherals display_service)

if(CONFIG_AUDIO_BOARD_CUSTOM)
message(STATUS "Current board name is SearaBoom custom")
list(APPEND COMPONENT_ADD_INCLUDEDIRS ./searaboom_board ./dummy_codec)
set(COMPONENT_SRCS
./searaboom_board/board.c
./searaboom_board/board_pins_config.c
./dummy_codec/dummy_codec.c
)
endif()

register_component()

IF (IDF_VERSION_MAJOR GREATER 3)
idf_component_get_property(audio_board_lib audio_board COMPONENT_LIB)
set_property(TARGET ${audio_board_lib} APPEND PROPERTY INTERFACE_LINK_LIBRARIES ${COMPONENT_LIB})
ENDIF (IDF_VERSION_MAJOR GREATER 3)
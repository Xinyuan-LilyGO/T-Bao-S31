# Board information output
message(STATUS "Selected Board: tbao_s31_board")
message(STATUS "Board Path: ../../boards/tbao_s31_board")

idf_component_register(
    SRC_DIRS "." "../../boards/tbao_s31_board"
    INCLUDE_DIRS "." "../../boards/tbao_s31_board"
    REQUIRES esp_board_manager
)

# This is equivalent to adding WHOLE_ARCHIVE option to the idf_component_register call above:
idf_component_set_property(${COMPONENT_NAME} WHOLE_ARCHIVE TRUE)

# ms800-emu: SDL2 front panel + audio, CoreMIDI ports, on top of ms800_core.
find_package(SDL2 REQUIRED)

ms800_embed_panel_art(PANEL_ART)

add_executable(ms800-emu
    main.c
    ui_panel.c
    ${PANEL_ART}
)
target_include_directories(ms800-emu PRIVATE . ${SDL2_INCLUDE_DIRS})
# Keep the binary (and its roms/ copy) at the top of the build directory.
set_target_properties(ms800-emu PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
target_link_libraries(ms800-emu PRIVATE ms800_core ${SDL2_LIBRARIES})

if(APPLE)
    target_link_libraries(ms800-emu PRIVATE
        "-framework CoreMIDI"
        "-framework CoreFoundation"
    )
endif()

add_custom_command(TARGET ms800-emu POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        ${PROJECT_SOURCE_DIR}/roms
        $<TARGET_FILE_DIR:ms800-emu>/roms
    COMMENT "Copying ROMs to build directory"
)

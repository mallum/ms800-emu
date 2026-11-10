# MS800 plugin: a CLAP written against ms800_core, wrapped to VST3 and AUv2
# by clap-wrapper. clap-wrapper and the SDKs it needs (CLAP, VST3,
# AudioUnitSDK) are downloaded at configure time.
if(NOT DEFINED CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()

include(FetchContent)
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES ON CACHE BOOL "Download SDKs for clap-wrapper" FORCE)
FetchContent_Declare(clap-wrapper
    GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
    GIT_TAG        806bba0a5f792e3966b99c1561ab9952d3316d7c   # main, 2026-10-20
)
FetchContent_MakeAvailable(clap-wrapper)

# The plugin implementation, as the static library clap-first expects.
add_library(ms800_clap_impl STATIC ms800_clap.c)
target_include_directories(ms800_clap_impl PUBLIC .)
target_link_libraries(ms800_clap_impl PUBLIC ms800_core clap)

# Editor (macOS): the front panel, drawn from the same embedded art as the
# standalone app; panel_layout.h comes from standalone/.
if(APPLE)
    ms800_embed_panel_art(PLUGIN_PANEL_ART)
    target_sources(ms800_clap_impl PRIVATE ms800_gui_mac.m ${PLUGIN_PANEL_ART})
    target_include_directories(ms800_clap_impl PRIVATE ${PROJECT_SOURCE_DIR}/standalone)
    set_source_files_properties(ms800_gui_mac.m PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(ms800_clap_impl PUBLIC "-framework Cocoa")
endif()

# ROM dumps are never committed, but a local build bundles any found in
# MS800_PLUGIN_ROM_DIR so the plugin works straight away (see ms800_clap.c for
# the run-time search order).
set(MS800_PLUGIN_RESOURCES ${CMAKE_BINARY_DIR}/plugin-resources)
file(MAKE_DIRECTORY ${MS800_PLUGIN_RESOURCES}/roms)
foreach(rom ms800-10-prog.bin ms800-waveform-rom.bin)
    if(EXISTS ${MS800_PLUGIN_ROM_DIR}/${rom})
        configure_file(${MS800_PLUGIN_ROM_DIR}/${rom} ${MS800_PLUGIN_RESOURCES}/roms/${rom} COPYONLY)
        message(STATUS "MS800: bundling ${rom} into the plugins")
    else()
        message(STATUS "MS800: ${rom} not found in ${MS800_PLUGIN_ROM_DIR}; plugins will look for it at run time")
    endif()
endforeach()

make_clapfirst_plugins(
    TARGET_NAME   ms800
    IMPL_TARGET   ms800_clap_impl
    OUTPUT_NAME   "MS800"
    ENTRY_SOURCE  ms800_clap_entry.cpp

    BUNDLE_IDENTIFIER "com.mallum.ms800"
    BUNDLE_VERSION    "0.1.0"

    COPY_AFTER_BUILD ${MS800_PLUGIN_INSTALL}
    PLUGIN_FORMATS   CLAP VST3 AUV2
    ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/plugins
    RESOURCE_DIRECTORY     ${MS800_PLUGIN_RESOURCES}

    AUV2_MANUFACTURER_NAME "mallum"
    AUV2_MANUFACTURER_CODE "Mllm"
    AUV2_SUBTYPE_CODE      "M800"
    AUV2_INSTRUMENT_TYPE   "aumu"
)

# clap-wrapper doesn't copy RESOURCE_DIRECTORY into AUv2 bundles yet.
if(APPLE AND TARGET ms800_auv2)
    add_custom_command(TARGET ms800_auv2 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${MS800_PLUGIN_RESOURCES} $<TARGET_BUNDLE_CONTENT_DIR:ms800_auv2>/Resources
        COMMENT "Copying plugin resources into MS800.component")
endif()

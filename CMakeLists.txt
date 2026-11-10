cmake_minimum_required(VERSION 3.16)
project(ms800-emu C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2")

option(MS800_BUILD_STANDALONE "Build the SDL2 standalone emulator" ON)
option(MS800_BUILD_PLUGIN "Build the CLAP/VST3/AUv2 plugin (downloads clap-wrapper and SDKs)" OFF)
option(MS800_PLUGIN_INSTALL "Copy the plugins into ~/Library/Audio/Plug-Ins after building" OFF)
set(MS800_PLUGIN_ROM_DIR "${PROJECT_SOURCE_DIR}/roms" CACHE PATH
    "ROM dumps to bundle into local plugin builds (never committed)")

if(MS800_BUILD_PLUGIN)
    # clap-wrapper is C++, and its macOS wrappers add Objective-C++ sources to
    # our targets, so these must be enabled here at the top level.
    enable_language(CXX)
    if(APPLE)
        enable_language(OBJC)
        enable_language(OBJCXX)
    endif()
endif()

# Compiles the panel artwork (rendered by tools/gen_panel_art.py) into C
# sources defining panel_bmp[] / sprites_bmp[]; returns their paths.
function(ms800_embed_panel_art out_var)
    set(embed_dir ${CMAKE_BINARY_DIR}/embedded)
    set(sources)
    foreach(art panel sprites)
        set(out ${embed_dir}/${art}_bmp.c)
        add_custom_command(
            OUTPUT ${out}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${embed_dir}
            COMMAND ${CMAKE_COMMAND} -DIN=${PROJECT_SOURCE_DIR}/assets/${art}.bmp
                    -DOUT=${out} -DNAME=${art}_bmp -P ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
            DEPENDS ${PROJECT_SOURCE_DIR}/assets/${art}.bmp ${PROJECT_SOURCE_DIR}/cmake/embed.cmake
            COMMENT "Embedding assets/${art}.bmp"
            VERBATIM)
        list(APPEND sources ${out})
    endforeach()
    set(${out_var} ${sources} PARENT_SCOPE)
endfunction()

add_subdirectory(core)

if(MS800_BUILD_STANDALONE)
    add_subdirectory(standalone)
endif()

if(MS800_BUILD_PLUGIN)
    add_subdirectory(plugin)
endif()

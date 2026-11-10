# ms800_core: the complete MS800 machine (6502 + firmware glue, M114S, ACIA,
# wave address latch, panel I/O). Plain C11, no platform dependencies.
add_library(ms800_core STATIC
    src/ms800.c
    src/m114s.c
    src/acia6850.c
    src/fake6502.c
)
target_include_directories(ms800_core
    PUBLIC  include
    PRIVATE src
)
# Linked into plugin bundles as well as executables.
set_target_properties(ms800_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(NOT MSVC)
    target_link_libraries(ms800_core PRIVATE m)
endif()

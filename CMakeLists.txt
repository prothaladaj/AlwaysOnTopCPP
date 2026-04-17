cmake_minimum_required(VERSION 3.16)
project(AlwaysOnTop CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ─── Resource compilation ─────────────────────────────────────────────────────
# Locate windres from the active MinGW toolchain prefix.
if(NOT DEFINED CMAKE_RC_COMPILER OR CMAKE_RC_COMPILER STREQUAL "")
    find_program(CMAKE_RC_COMPILER
        NAMES
            ${TOOLCHAIN_PREFIX}windres
            x86_64-w64-mingw32-windres
            windres
        REQUIRED)
endif()

set(RC_SRC  "${CMAKE_SOURCE_DIR}/res/app.rc")
set(RC_OBJ  "${CMAKE_BINARY_DIR}/app_rc.o")

add_custom_command(
    OUTPUT  ${RC_OBJ}
    COMMAND ${CMAKE_RC_COMPILER}
            --input  ${RC_SRC}
            --output ${RC_OBJ}
            --output-format=coff
            -I ${CMAKE_SOURCE_DIR}/res
    DEPENDS ${RC_SRC}
            ${CMAKE_SOURCE_DIR}/res/resource.h
            ${CMAKE_SOURCE_DIR}/res/app.manifest
            ${CMAKE_SOURCE_DIR}/res/icon_idle.ico
            ${CMAKE_SOURCE_DIR}/res/icon_active.ico
    COMMENT "Compiling Windows resources"
)
add_custom_target(resources DEPENDS ${RC_OBJ})

# ─── Executable ───────────────────────────────────────────────────────────────
add_executable(AlwaysOnTop WIN32
    src/main.cpp
    ${RC_OBJ}          # pre-compiled resource object
)
add_dependencies(AlwaysOnTop resources)

target_include_directories(AlwaysOnTop PRIVATE res)

# ─── Compiler / linker flags ──────────────────────────────────────────────────
if(MINGW OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(AlwaysOnTop PRIVATE
        -Os           # optimise for size
        -Wall
        -Wextra
        -fno-exceptions
        -fno-rtti
    )
    target_link_options(AlwaysOnTop PRIVATE
        -mwindows     # WIN32 subsystem, no console
        -s            # strip symbols → smaller .exe
        -static-libgcc
        -static-libstdc++
    )
elseif(MSVC)
    target_compile_options(AlwaysOnTop PRIVATE
        /W3 /O1 /GS- /GR-
    )
    set_target_properties(AlwaysOnTop PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF")
endif()

target_link_libraries(AlwaysOnTop PRIVATE
    user32 shell32 advapi32 comctl32
)

# ─── Install ──────────────────────────────────────────────────────────────────
install(TARGETS AlwaysOnTop DESTINATION .)

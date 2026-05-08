cmake_minimum_required(VERSION 3.20)

project(playing_with_lsystems VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(LSYSTEMS_STRICT "Enable stricter warning sets." ON)
option(LSYSTEMS_WARN_AS_ERROR "Treat warnings as errors." ON)

find_package(SDL3 3.4.0 CONFIG QUIET)

if (NOT SDL3_FOUND)
    set(LSYSTEMS_LOCAL_SDL3_CONFIG
        "/home/vega/Coding/Graphics/hardware-renderer-cpp/build/debug/_deps/sdl3-build"
    )
    if (EXISTS "${LSYSTEMS_LOCAL_SDL3_CONFIG}/SDL3Config.cmake")
        list(PREPEND CMAKE_PREFIX_PATH "${LSYSTEMS_LOCAL_SDL3_CONFIG}")
        find_package(SDL3 3.4.0 CONFIG QUIET)
    endif()
endif()

if (NOT SDL3_FOUND)
    set(SDL_X11_XTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        SDL3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.4.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL3)
endif()

add_executable(lsystems
    src/lsystem.cpp
    src/main.cpp
    src/turtle.cpp
)

target_compile_features(lsystems PRIVATE cxx_std_20)
target_link_libraries(lsystems PRIVATE SDL3::SDL3)

target_compile_definitions(lsystems
    PRIVATE
        LSYSTEMS_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples"
)

if (MSVC)
    target_compile_options(lsystems PRIVATE /W4 /permissive-)
    if (LSYSTEMS_WARN_AS_ERROR)
        target_compile_options(lsystems PRIVATE /WX)
    endif()
else()
    target_compile_options(
        lsystems
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            $<$<BOOL:${LSYSTEMS_STRICT}>:-Wconversion>
            $<$<BOOL:${LSYSTEMS_STRICT}>:-Wsign-conversion>
            $<$<BOOL:${LSYSTEMS_STRICT}>:-Wshadow>
            $<$<BOOL:${LSYSTEMS_WARN_AS_ERROR}>:-Werror>
    )
endif()

set_target_properties(lsystems PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

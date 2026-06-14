cmake_minimum_required(VERSION 3.20)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" LLM_USAGE_TRAY_VERSION LIMIT_COUNT 1)
string(STRIP "${LLM_USAGE_TRAY_VERSION}" LLM_USAGE_TRAY_VERSION)

project(LLMUsageTray VERSION ${LLM_USAGE_TRAY_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG 7bbd9d5c2c6925e5824de4288a1ed0bed0c0e5f7
)
FetchContent_MakeAvailable(SDL)

set(SDLTTF_SAMPLES OFF CACHE BOOL "" FORCE)
set(SDLTTF_INSTALL OFF CACHE BOOL "" FORCE)
set(SDLTTF_VENDORED ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    SDL_ttf
    GIT_REPOSITORY https://github.com/libsdl-org/SDL_ttf.git
    GIT_TAG 1abbd07aeda8da762d044dda6c247588e2ef9fc1
)
FetchContent_MakeAvailable(SDL_ttf)

set(LLM_USAGE_TRAY_SOURCES
    src/main.cpp
    src/base64url.cpp
    src/json_util.cpp
    src/oauth.cpp
    src/usage.cpp
)

if(WIN32)
    list(APPEND LLM_USAGE_TRAY_SOURCES
        src/platform/windows/credential_store.cpp
        src/platform/windows/http_client.cpp
        src/platform/windows/local_callback_server.cpp
        src/platform/windows/platform.cpp
    )
elseif(APPLE)
    find_package(CURL REQUIRED)
    list(APPEND LLM_USAGE_TRAY_SOURCES
        src/platform/macos/credential_store.cpp
        src/platform/unix/http_client.cpp
        src/platform/unix/local_callback_server.cpp
        src/platform/unix/platform.cpp
    )
else()
    find_package(CURL REQUIRED)
    list(APPEND LLM_USAGE_TRAY_SOURCES
        src/platform/linux/credential_store.cpp
        src/platform/unix/http_client.cpp
        src/platform/unix/local_callback_server.cpp
        src/platform/unix/platform.cpp
    )
endif()

if(WIN32)
    add_executable(LLMUsageTray WIN32 ${LLM_USAGE_TRAY_SOURCES})
else()
    add_executable(LLMUsageTray ${LLM_USAGE_TRAY_SOURCES})
endif()

target_include_directories(LLMUsageTray PRIVATE src)
target_compile_definitions(LLMUsageTray PRIVATE LLM_USAGE_TRAY_VERSION="${PROJECT_VERSION}")
target_link_libraries(LLMUsageTray PRIVATE SDL3::SDL3 SDL3_ttf::SDL3_ttf)

if(WIN32)
    target_sources(LLMUsageTray PRIVATE resources/app.rc)
    target_compile_definitions(LLMUsageTray PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(LLMUsageTray PRIVATE
        winhttp
        crypt32
        bcrypt
        advapi32
        ws2_32
        shell32
    )
else()
    target_link_libraries(LLMUsageTray PRIVATE CURL::libcurl)
endif()

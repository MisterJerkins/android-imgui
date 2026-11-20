cmake_minimum_required(VERSION 3.22)
project(android-imgui VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# The Java helper run by app_process: system clipboard, touches where no touchscreen can be
# read, keeping the screen awake. Without it the binary carries no dex and starts no process.
option(ANDROID_IMGUI_HELPER "Carry the Java helper inside the binary" ON)

# These come before the first target: a target takes the options that are set when it is made.
if(ANDROID)
    # std::jthread and std::stop_token are still behind this switch in the NDK's libc++.
    add_compile_options(-fexperimental-library)
    add_link_options(-fexperimental-library)
    add_compile_options(-fno-exceptions -fno-rtti -fvisibility=hidden -ffunction-sections -fdata-sections "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
    add_link_options(-Wl,--gc-sections)
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
        add_link_options(-s)
    endif()
endif()

# Dear ImGui, as it comes: only its build options are ours.
add_library(imgui STATIC
        dependencies/imgui/imgui.cpp
        dependencies/imgui/imgui_draw.cpp
        dependencies/imgui/imgui_tables.cpp
        dependencies/imgui/imgui_widgets.cpp)
target_include_directories(imgui SYSTEM PUBLIC dependencies/imgui)
target_compile_definitions(imgui PUBLIC
        IMGUI_DISABLE_OBSOLETE_FUNCTIONS
        IMGUI_DISABLE_DEMO_WINDOWS
        IMGUI_DISABLE_DEBUG_TOOLS
        IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
list(FILTER SOURCES EXCLUDE REGEX "/src/Main[.]cpp$")

# What needs Android itself. Everything else also builds on the development machine, where the
# unit tests run.
set(ANDROID_ONLY "/src/(Helper|Graphics/OpenGL|Graphics/Vulkan)/")
string(APPEND ANDROID_ONLY "|/src/Utils/System[.]cpp$")
string(APPEND ANDROID_ONLY "|/src/Window/(ANativeWindowCreator|LibGui)[.]cpp$")
string(APPEND ANDROID_ONLY "|/src/Graphics/(Graphics|Software/SoftwareGraphics)[.]cpp$")
string(APPEND ANDROID_ONLY "|/src/Input/Touch/TouchReader[.]cpp$")

# The library an application links: `target_link_libraries(app PRIVATE android_imgui)`.
if(ANDROID)
    add_library(android_imgui STATIC ${SOURCES})
    target_compile_options(android_imgui PRIVATE -Wall -Wextra)
    if(ANDROID_IMGUI_HELPER)
        target_compile_definitions(android_imgui PRIVATE ANDROID_IMGUI_HELPER)
    endif()
    target_link_libraries(android_imgui PUBLIC imgui log android EGL GLESv2 dl)
else()
    list(FILTER SOURCES EXCLUDE REGEX "${ANDROID_ONLY}")
    add_library(android_imgui STATIC ${SOURCES})
    target_link_libraries(android_imgui PUBLIC imgui)
    if(MSVC)
        target_compile_options(android_imgui PUBLIC /W4 /utf-8 /permissive- /Zc:preprocessor)
        target_compile_definitions(android_imgui PUBLIC _CRT_SECURE_NO_WARNINGS NOMINMAX)
    endif()
endif()
target_include_directories(android_imgui PUBLIC src)
target_include_directories(android_imgui SYSTEM PRIVATE dependencies/stb)

# The demo and the tests are this project's own: one that takes it in with add_subdirectory
# gets the library alone.
if(PROJECT_IS_TOP_LEVEL)
    if(ANDROID)
        add_executable(android-imgui src/Main.cpp)
        target_compile_options(android-imgui PRIVATE -Wall -Wextra)
        target_link_libraries(android-imgui PRIVATE android_imgui)

        # What the device tests run on the device, where a copy of the sources has it.
        file(GLOB SELFTEST CONFIGURE_DEPENDS tests/Device/*.cpp)
        if(SELFTEST)
            add_executable(selftest ${SELFTEST})
            target_compile_options(selftest PRIVATE -Wall -Wextra)
            target_link_libraries(selftest PRIVATE android_imgui)
        endif()
    endif()

    # The unit tests run on the development machine and, built for Android, on a device as well.
    file(GLOB UNIT_TESTS CONFIGURE_DEPENDS tests/Unit/*.cpp)
    add_executable(unit-tests ${UNIT_TESTS})
    target_include_directories(unit-tests PRIVATE tests/Unit)
    target_link_libraries(unit-tests PRIVATE android_imgui)
endif()

cmake_minimum_required(VERSION 3.20)
project(PhantomClient LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT WIN32)
    message(FATAL_ERROR "PhantomClient is Windows-only.")
endif()

# ---------------------------------------------------------------
# JDK (needed for jni.h and jvmti.h)
# Override with -DJDK_PATH="C:/path/to/jdk"
# ---------------------------------------------------------------
set(JDK_PATH "C:/Program Files/Java/jdk1.8.0_202" CACHE PATH "Path to a JDK 8 install")

if(NOT EXISTS "${JDK_PATH}/include/jni.h")
    message(FATAL_ERROR
        "jni.h not found under ${JDK_PATH}/include.\n"
        "Point CMake at a real JDK: -DJDK_PATH=\"C:/Program Files/Java/jdk1.8.0_202\"")
endif()

include(FetchContent)

# ---------------------------------------------------------------
# MinHook
# ---------------------------------------------------------------
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
    GIT_TAG        master
)
FetchContent_MakeAvailable(minhook)

# ---------------------------------------------------------------
# Dear ImGui
# ImGui ships no CMakeLists.txt, so point SOURCE_SUBDIR at a path
# that does not exist. FetchContent then downloads the sources but
# skips add_subdirectory instead of erroring out.
# ---------------------------------------------------------------
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.90.4
    SOURCE_SUBDIR  _no_cmake_here
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

# ---------------------------------------------------------------
# The client
#
# Every module lives in a header and is fully inline, so the client
# is a single translation unit. The old per-module .cpp stubs are
# kept in the tree for reference but are not compiled: including
# each header twice would only slow the build down.
# ---------------------------------------------------------------
add_library(PhantomClient SHARED
    src/dllmain.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(PhantomClient PRIVATE
    src
    "${JDK_PATH}/include"
    "${JDK_PATH}/include/win32"
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(PhantomClient PRIVATE
    minhook
    opengl32
)

target_compile_definitions(PhantomClient PRIVATE
    _CRT_SECURE_NO_WARNINGS
    WIN32_LEAN_AND_MEAN
    NOMINMAX            # windows.h min/max macros break std::min/std::max
)

if(MSVC)
    target_compile_options(PhantomClient PRIVATE /MP /W3)
    # Static CRT so the DLL runs without a redistributable installed
    set_property(TARGET PhantomClient PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set_target_properties(PhantomClient PROPERTIES
    OUTPUT_NAME "PhantomClient"
    PREFIX ""
)

message(STATUS "JDK:   ${JDK_PATH}")
message(STATUS "ImGui: ${imgui_SOURCE_DIR}")

cmake_minimum_required(VERSION 3.20)
project(PhantomClient LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- JDK path (adjust to your system) ---
set(JDK_PATH "C:/Program Files/Java/jdk1.8.0_202" CACHE PATH "Path to JDK")

# --- MinHook ---
include(FetchContent)
FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/TsudaKageWorker/minhook.git
    GIT_TAG master
)
FetchContent_MakeAvailable(minhook)

# --- ImGui ---
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.90.4
)
FetchContent_MakeAvailable(imgui)

set(IMGUI_SOURCES
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl2.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
)

# --- Main DLL ---
add_library(PhantomClient SHARED
    src/dllmain.cpp
    src/jni/jni_helper.cpp
    src/jni/class_resolver.cpp
    src/mc/minecraft.cpp
    src/hooks/gl_hook.cpp
    src/modules/module_manager.cpp
    src/modules/combat/aim_assist.cpp
    src/modules/combat/kill_aura.cpp
    src/modules/combat/velocity.cpp
    src/modules/movement/speed.cpp
    src/modules/movement/sprint.cpp
    src/modules/movement/fly.cpp
    src/modules/visual/esp.cpp
    src/modules/visual/fullbright.cpp
    src/gui/menu.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(PhantomClient PRIVATE
    src
    ${JDK_PATH}/include
    ${JDK_PATH}/include/win32
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
)

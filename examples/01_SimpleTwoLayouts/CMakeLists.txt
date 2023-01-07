cmake_minimum_required(VERSION 3.5)
set(MY_APP_NAME "SimpleTwoLayouts")
project(SimpleTwoLayouts VERSION 0.1 LANGUAGES CXX)

include_directories("$ENV{VULKAN_SDK}/Include")
include_directories(../../import/glfw/include)
include_directories(../../)

set(DearImGUIPath ../../import/imgui)
include_directories(${DearImGUIPath})
include_directories(${DearImGUIPath}/backends)

link_directories("$ENV{VULKAN_SDK}/lib")
link_directories("../../import/glfw/build/src/Debug/")

add_definitions(-DSOURCE_PATH=\"${CMAKE_CURRENT_SOURCE_DIR}\")
add_executable(${MY_APP_NAME} "main.cpp"
                              ../../CustomDearImGuiLayout.h
                              ${DearImGUIPath}/imgui.cpp
                              ${DearImGUIPath}/imgui_draw.cpp
                              ${DearImGUIPath}/imgui_demo.cpp
                              ${DearImGUIPath}/imgui_tables.cpp
                              ${DearImGUIPath}/imgui_widgets.cpp
                              ${DearImGUIPath}/backends/imgui_impl_vulkan.cpp
                              ${DearImGUIPath}/backends/imgui_impl_glfw.cpp)

target_compile_features(${MY_APP_NAME} PRIVATE cxx_std_17)
target_link_libraries(${MY_APP_NAME} vulkan-1)
target_link_libraries(${MY_APP_NAME} glfw3)

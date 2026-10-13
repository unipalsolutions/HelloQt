cmake_minimum_required(VERSION 3.16)

project(HelloQt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

qt_standard_project_setup()

qt_add_executable(HelloQt
    src/main.cpp
    src/mainwindow.h
    src/mainwindow.cpp
)

target_link_libraries(HelloQt PRIVATE Qt6::Widgets)

# ترميز UTF-8 للملفات المصدرية مع MSVC
if(MSVC)
    target_compile_options(HelloQt PRIVATE /utf-8)
endif()

set_target_properties(HelloQt PROPERTIES
    WIN32_EXECUTABLE ON
)

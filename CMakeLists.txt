cmake_minimum_required(VERSION 3.21)

project(classiCAD VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

add_executable(classiCAD
    src/main.cpp
)

target_link_libraries(classiCAD PRIVATE
    Qt${QT_VERSION_MAJOR}::Widgets
)

if (MSVC)
    target_compile_options(classiCAD PRIVATE /W4)
else()
    target_compile_options(classiCAD PRIVATE -Wall -Wextra -Wpedantic)
endif()

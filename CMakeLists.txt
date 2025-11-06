cmake_minimum_required(VERSION 3.1.0)

project(chmreader VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Qt5 packages
find_package(Qt5 COMPONENTS Core Gui Widgets WebEngineWidgets REQUIRED)

set(PROJECT_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
)

add_executable(chmreader ${PROJECT_SOURCES})

target_link_libraries(chmreader
    Qt5::Core
    Qt5::Gui
    Qt5::Widgets
    Qt5::WebEngineWidgets
)

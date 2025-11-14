cmake_minimum_required(VERSION 3.16)

project(photoroom VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Packages
find_package(Qt6 COMPONENTS Widgets Sql Concurrent OpenGLWidgets REQUIRED)
find_package(LibRaw CONFIG REQUIRED)

# Define all your source, header, and UI files in one place.
set(PROJECT_SOURCES
    main.cpp
    mainwindow.cpp
    mainwindow.h
    mainwindow.ui
    preferencesdialog.cpp
    preferencesdialog.h
    preferencesdialog.ui
    imagelabel.cpp
    imagelabel.h
    imageloader.cpp
    imageloader.h
    librarygridview.cpp
    librarygridview.h
    librarymanager.cpp
    librarymanager.h
    previewgenerator.cpp
    previewgenerator.h
    histogramwidget.cpp
    histogramwidget.h
    jobmanager.cpp
    jobmanager.h
    jobspanel.cpp
    jobspanel.h
    jobswindow.cpp
    jobswindow.h
    exportdialog.cpp
    exportdialog.h
    developadjustmentengine.cpp
    developadjustmentengine.h
    developtypes.cpp
    developtypes.h
    importpreviewdialog.cpp
    importpreviewdialog.h
)

# Create the executable using the list of sources.
qt_add_executable(photoroom
    ${PROJECT_SOURCES}
)

target_link_libraries(photoroom PRIVATE
    Qt6::Widgets
    Qt6::Sql
    Qt6::Concurrent
    Qt6::OpenGLWidgets
    Qt6::Gui
    libraw::raw
)

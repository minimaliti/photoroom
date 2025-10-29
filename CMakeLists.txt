cmake_minimum_required(VERSION 3.16)

project(photoroom VERSION 0.1 LANGUAGES CXX)

set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Qt6 and its components. This is the only find_package needed.
find_package(Qt6 COMPONENTS Widgets Concurrent REQUIRED)

set(PROJECT_SOURCES
        main.cpp
        mainwindow.cpp
        mainwindow.h
        mainwindow.ui
        imagelabel.h
        imagelabel.cpp
        preferencesdialog.h
        preferencesdialog.cpp
        preferencesdialog.ui
        imageprocessor.h
        imageprocessor.cpp
        imageadjustments.h
        librarymanager.h
        librarymanager.cpp
        importpreviewdialog.h
        importpreviewdialog.cpp
        importpreviewdialog.ui
)

# Since we require Qt6 above, we can use qt_add_executable directly
# without the need for an if/else block.
qt_add_executable(photoroom
    MANUAL_FINALIZATION
    ${PROJECT_SOURCES}
)

# Link against the Qt libraries
target_link_libraries(photoroom PRIVATE
    Qt::Widgets
    Qt::Concurrent
)

# --- START OF LIBRAW SUPPORT ---

set(LIBRAW_DIR ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty/libraw)

find_library(LIBRAW_LIBRARY
             NAMES raw_r raw
             PATHS ${LIBRAW_DIR}/lib
             NO_DEFAULT_PATH)

find_file(LIBRAW_DLL
          NAMES libraw_r.dll libraw.dll
          PATHS ${LIBRAW_DIR}/bin
          NO_DEFAULT_PATH)

if(NOT LIBRAW_LIBRARY)
    message(FATAL_ERROR "LibRaw library could not be found in ${LIBRAW_DIR}/lib. Searched for 'raw_r' and 'raw'. Please verify 'libraw_r.dll.a' exists.")
else()
    message(STATUS "Found LibRaw library: ${LIBRAW_LIBRARY}")
    target_include_directories(photoroom PRIVATE ${LIBRAW_DIR}/include)
    target_link_libraries(photoroom PRIVATE ${LIBRAW_LIBRARY})
endif()

if(LIBRAW_DLL)
    message(STATUS "Found LibRaw DLL: ${LIBRAW_DLL}")
    add_custom_command(TARGET photoroom POST_BUILD
                       COMMAND ${CMAKE_COMMAND} -E copy_if_different
                       "${LIBRAW_DLL}"
                       "$<TARGET_FILE_DIR:photoroom>")
else()
    message(WARNING "Could not find libraw_r.dll or libraw.dll in ${LIBRAW_DIR}/bin. The application will compile but will likely fail to run.")
endif()

# --- END OF LIBRAW SUPPORT ---

set_target_properties(photoroom PROPERTIES
    ${BUNDLE_ID_OPTION}
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
    MACOSX_BUNDLE TRUE
    WIN32_EXECUTABLE TRUE
)

include(GNUInstallDirs)
install(TARGETS photoroom
    BUNDLE DESTINATION .
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Finalize the executable (a Qt6 command). No 'if' statement is needed.
qt_finalize_executable(photoroom)

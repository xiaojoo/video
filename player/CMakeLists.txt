cmake_minimum_required(VERSION 3.22)
project(Playerui VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Quick QuickControls2 Svg Sql QuickDialogs2)
qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(appPlayerui
    src/main.cpp
    src/playercore.h
    src/playercore.cpp
    src/videohost.h
    src/videohost.cpp
    src/videoitem.h
    src/videoitem.cpp
    src/library.h
    src/library.cpp
    src/bluray.h
    src/bluray.cpp
    src/duration.h
    src/duration.cpp
    src/singleinstance.h
    src/singleinstance.cpp
    src/thumbstrip.h
    src/thumbstrip.cpp
    appPlayerui.rc
)

# The window icon and the exe's file icon are the same app.ico (16 and 24 are a simplified
# cut of the figure; see icons/app_small.svg), and icons/make.py regenerates every frame
# from the two SVGs.
qt_add_resources(appPlayerui "appicons"
    PREFIX "/app"
    BASE "icons"
    FILES "icons/app.ico"
)

set(LIBMPV_DIR "${CMAKE_SOURCE_DIR}/../vendor/libmpv")
target_include_directories(appPlayerui PRIVATE "${LIBMPV_DIR}/include")
# The RHI headers are not on the default include path: VideoItem needs
# <rhi/qrhi.h> and <rhi/qrhi_platform.h> to reach the window's own D3D11 device.
# <videoitem.h> also has to be findable by angle brackets, because that is how the
# generated qmltyperegistrations.cpp includes it - without src/ on the path the
# __has_include guard fails and VideoItem is registered but never declared.
get_target_property(QT_GUI_INCLUDE_DIRS Qt6::Gui INTERFACE_INCLUDE_DIRECTORIES)
list(GET QT_GUI_INCLUDE_DIRS 0 QT_GUI_INCLUDE_DIR)
target_include_directories(appPlayerui PRIVATE
    "${QT_GUI_INCLUDE_DIR}/${Qt6_VERSION}/QtGui"
    "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(appPlayerui PRIVATE "${LIBMPV_DIR}/libmpv-2.lib")
add_custom_command(TARGET appPlayerui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LIBMPV_DIR}/libmpv-2.dll" $<TARGET_FILE_DIR:appPlayerui>)

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_qml_module(appPlayerui
    URI Playerui
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/Icon.qml
        qml/ScrubBar.qml
        qml/ControlRow.qml
        qml/HoverCard.qml
        qml/Tip.qml
        qml/Picker.qml
        qml/CaptionButtons.qml
        qml/LibraryPage.qml
        qml/ContextMenu.qml
        qml/Playlist.qml
)

target_link_libraries(appPlayerui PRIVATE Qt6::Quick Qt6::Svg Qt6::QuickControls2 Qt6::Sql Qt6::QuickDialogs2 user32 gdi32 dwmapi d3d11 comctl32 shell32)

# Dev build: console subsystem so qml errors reach the terminal. Packaging
# configures with -DSHIP_WIN32=ON so double-clicking does not open a console.
option(SHIP_WIN32 "Build the windowed (no console) variant for packaging" OFF)
# The build target keeps its appPlayerui name (the QML module, the .rc and the
# qmlcache files are all keyed on it); only the file on disk is Player.exe.
set_target_properties(appPlayerui PROPERTIES WIN32_EXECUTABLE ${SHIP_WIN32} OUTPUT_NAME Player)

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
)

set(LIBMPV_DIR "${CMAKE_SOURCE_DIR}/../vendor/libmpv")
target_include_directories(appPlayerui PRIVATE "${LIBMPV_DIR}/include")
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
        qml/ChapterList.qml
)

target_link_libraries(appPlayerui PRIVATE Qt6::Quick Qt6::Svg Qt6::QuickControls2 Qt6::Sql Qt6::QuickDialogs2 user32)

# Dev build: console subsystem so qml errors reach the terminal. Flip back to ON
# before shipping so double-clicking does not open a console window.
set_target_properties(appPlayerui PROPERTIES WIN32_EXECUTABLE OFF)

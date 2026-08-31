# monix-qt-ui — Qt 6 Quick/QML frontend for the Monix fork (UI-STACK.md §5).
#
# Current state: hello-world shell — Qt only, no lib/monix, no ALSA.
# Phase 1 adds the DeviceBackend split per DEV-WORKFLOW.md §3:
#   - Linux: link the real static lib/monix (needs ALSA, pkg-config)
#   - macOS / CI: build the MockBackend instead (no ALSA, no hardware)

cmake_minimum_required(VERSION 3.16)
project(monix-qt-ui VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick)   # UI-STACK.md §9: min Qt 6.5

qt_policy(SET QTP0001 NEW)   # ':/qt/qml/' resource prefix for QML modules

qt_standard_project_setup()

qt_add_executable(monix-qt-ui
    main.cpp
)

qt_add_qml_module(monix-qt-ui
    URI Monix
    VERSION 1.0
    QML_FILES
        Main.qml
)

target_link_libraries(monix-qt-ui PRIVATE Qt6::Quick Qt6::Gui)

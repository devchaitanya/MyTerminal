cmake_minimum_required(VERSION 3.16)
project(MyTerminal LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(X11 REQUIRED)

add_library(terminal_gui
    src/gui/TerminalWindow.cpp
    src/gui/Tab.cpp
    src/core/History.cpp
)
target_include_directories(terminal_gui PUBLIC include ${X11_INCLUDE_DIR})
target_link_libraries(terminal_gui PUBLIC ${X11_LIBRARIES})

add_executable(myshell src/app/main.cpp)
target_include_directories(myshell PRIVATE include)
target_link_libraries(myshell PRIVATE terminal_gui)

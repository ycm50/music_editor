add_executable(ui main.cpp mainwindow.cpp)
# GUI 子系统 (MinGW: -mwindows), 启动时不弹终端窗口
set_target_properties(ui PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_libraries(ui PRIVATE
    music-core
    Qt${QT_VERSION_MAJOR}::Widgets
    Qt${QT_VERSION_MAJOR}::Network
)

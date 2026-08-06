add_executable(player main.cpp)
target_link_libraries(player PRIVATE
    music-core
    Qt${QT_VERSION_MAJOR}::Multimedia
)

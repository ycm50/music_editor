add_library(music-core STATIC
    note_parser.cpp
    tone_gen.cpp
    wav_writer.cpp
)

target_include_directories(music-core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(music-core PROPERTIES AUTOMOC OFF)

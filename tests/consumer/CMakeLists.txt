cmake_minimum_required(VERSION 3.23)

project(ScopeWriterConsumer LANGUAGES CXX)

include(CTest)

find_package(ScopeWriter CONFIG REQUIRED)

add_executable(ScopeWriterConsumer main.cpp)
target_compile_features(ScopeWriterConsumer PRIVATE cxx_std_20)
target_link_libraries(ScopeWriterConsumer PRIVATE ScopeWriter::ScopeWriter)

if (WIN32)
    add_custom_command(TARGET ScopeWriterConsumer POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:ScopeWriter::ScopeWriter>
                    $<TARGET_FILE_DIR:ScopeWriterConsumer>
    )
endif ()

add_test(NAME ScopeWriterInstalledConsumer
        COMMAND ScopeWriterConsumer
                "${CMAKE_CURRENT_BINARY_DIR}/consumer.ome.tiff"
)

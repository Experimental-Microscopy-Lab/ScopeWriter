cmake_minimum_required(VERSION 3.23)

project(ScopeWriter VERSION 0.2.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(SCOPEWRITER_BUILD_TESTS "Build ScopeWriter tests" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
find_package(ZLIB REQUIRED)
find_package(zstd CONFIG REQUIRED)
find_package(TIFF REQUIRED)
find_package(Crc32c CONFIG REQUIRED)
find_package(Threads REQUIRED)
set(_scopewriter_zarr_libraries
        Crc32c::crc32c
        zstd::libzstd
        Threads::Threads
)

add_library(ScopeWriter SHARED
        src/DatasetFrame.cpp
        src/ScopeWriter.cpp
        src/ZarrWriter.cpp
        src/ZarrWriter.h
        src/zarr/Chunk.cpp
        src/zarr/Chunk.h
        src/zarr/Downsample.cpp
        src/zarr/Downsample.h
        src/zarr/FileHandle.cpp
        src/zarr/FileHandle.h
        src/zarr/FileSink.cpp
        src/zarr/FileSink.h
        src/zarr/FrameQueue.cpp
        src/zarr/FrameQueue.h
        src/zarr/Shard.cpp
        src/zarr/Shard.h
        src/zarr/ThreadPool.cpp
        src/zarr/ThreadPool.h
        include/scopewriter/ScopeWriter.h
        include/scopewriter/scopewriter_export.h
)
add_library(ScopeWriter::ScopeWriter ALIAS ScopeWriter)
set_target_properties(ScopeWriter PROPERTIES
        AUTOMOC OFF
        AUTORCC OFF
        AUTOUIC OFF
        VERSION "${PROJECT_VERSION}"
        SOVERSION "${PROJECT_VERSION_MAJOR}"
)

target_compile_features(ScopeWriter PUBLIC cxx_std_20)
target_compile_definitions(ScopeWriter PRIVATE SCOPEWRITER_EXPORTS SCOPEWRITER_VERSION_STRING="${PROJECT_VERSION}")

target_include_directories(ScopeWriter
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(ScopeWriter PRIVATE TIFF::TIFF ZLIB::ZLIB)
target_link_libraries(ScopeWriter PRIVATE ${_scopewriter_zarr_libraries})

if (MSVC)
    target_compile_options(ScopeWriter PRIVATE /W4 /permissive-)
else ()
    target_compile_options(ScopeWriter PRIVATE -Wall -Wextra -Wpedantic)
endif ()

if (SCOPEWRITER_BUILD_TESTS)
    enable_testing()
    add_executable(ScopeWriterTests tests/ScopeWriterTests.cpp)
    set_target_properties(ScopeWriterTests PROPERTIES
            AUTOMOC OFF
            AUTORCC OFF
            AUTOUIC OFF
    )
    target_compile_features(ScopeWriterTests PRIVATE cxx_std_20)
    target_link_libraries(ScopeWriterTests PRIVATE
            ScopeWriter
            TIFF::TIFF
            ${_scopewriter_zarr_libraries}
    )
    add_test(NAME ScopeWriterTests COMMAND ScopeWriterTests)
endif ()

configure_package_config_file(
        cmake/ScopeWriterConfig.cmake.in
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeWriterConfig.cmake"
        INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeWriter"
)
write_basic_package_version_file(
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeWriterConfigVersion.cmake"
        VERSION "${PROJECT_VERSION}"
        COMPATIBILITY SameMajorVersion
)

install(TARGETS ScopeWriter
        EXPORT ScopeWriterTargets
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
        LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
        INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES README.md LICENSE THIRD_PARTY_NOTICES.md
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/ScopeWriter"
)
install(EXPORT ScopeWriterTargets
        FILE ScopeWriterTargets.cmake
        NAMESPACE ScopeWriter::
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeWriter"
)
export(EXPORT ScopeWriterTargets
        FILE "${CMAKE_CURRENT_BINARY_DIR}/ScopeWriterTargets.cmake"
        NAMESPACE ScopeWriter::
)
install(FILES
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeWriterConfig.cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/ScopeWriterConfigVersion.cmake"
        DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ScopeWriter"
)

cmake_minimum_required(VERSION 3.23)

project(ScopeWriter VERSION 0.1.0 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(SCOPEWRITER_BUILD_TESTS "Build ScopeWriter tests" ${PROJECT_IS_TOP_LEVEL})

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(_scopewriter_third_party_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON)
add_subdirectory(
        "${_scopewriter_third_party_dir}/zlib"
        "${CMAKE_CURRENT_BINARY_DIR}/third_party/zlib"
        EXCLUDE_FROM_ALL
)
unset(SKIP_INSTALL_ALL)
add_library(ZLIB::ZLIB ALIAS zlibstatic)

set(CRC32C_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CRC32C_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(CRC32C_USE_GLOG OFF CACHE BOOL "" FORCE)
set(CRC32C_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(
        "${_scopewriter_third_party_dir}/crc32c"
        "${CMAKE_CURRENT_BINARY_DIR}/third_party/crc32c"
        EXCLUDE_FROM_ALL
)
add_library(Crc32c::crc32c ALIAS crc32c)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC ON CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)
set(ZSTD_MULTITHREAD_SUPPORT OFF CACHE BOOL "" FORCE)
add_subdirectory(
        "${_scopewriter_third_party_dir}/zstd/build/cmake"
        "${CMAKE_CURRENT_BINARY_DIR}/third_party/zstd"
        EXCLUDE_FROM_ALL
)
add_library(zstd::libzstd_static ALIAS libzstd_static)

set(tiff-static ON CACHE BOOL "" FORCE)
set(tiff-tools OFF CACHE BOOL "" FORCE)
set(tiff-tests OFF CACHE BOOL "" FORCE)
set(tiff-contrib OFF CACHE BOOL "" FORCE)
set(tiff-docs OFF CACHE BOOL "" FORCE)
set(tiff-install OFF CACHE BOOL "" FORCE)
set(tiff-cxx OFF CACHE BOOL "" FORCE)
set(zlib ON CACHE BOOL "" FORCE)
set(libdeflate OFF CACHE BOOL "" FORCE)
set(jpeg OFF CACHE BOOL "" FORCE)
set(old-jpeg OFF CACHE BOOL "" FORCE)
set(jpeg12 OFF CACHE BOOL "" FORCE)
set(jbig OFF CACHE BOOL "" FORCE)
set(lerc OFF CACHE BOOL "" FORCE)
set(lzma OFF CACHE BOOL "" FORCE)
set(zstd OFF CACHE BOOL "" FORCE)
set(webp OFF CACHE BOOL "" FORCE)
add_subdirectory(
        "${_scopewriter_third_party_dir}/libtiff"
        "${CMAKE_CURRENT_BINARY_DIR}/third_party/libtiff"
        EXCLUDE_FROM_ALL
)

find_package(Threads REQUIRED)
set(_scopewriter_zarr_libraries
        Crc32c::crc32c
        zstd::libzstd_static
        Threads::Threads
)

add_library(ScopeWriter SHARED
        src/ScopeWriter.cpp
        src/ZarrWriter.cpp
        src/ZarrWriter.h
        src/zarr/Chunk.cpp
        src/zarr/Chunk.h
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
target_compile_definitions(ScopeWriter PRIVATE SCOPEWRITER_EXPORTS)

target_include_directories(ScopeWriter
        PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(ScopeWriter PRIVATE TIFF::tiff)
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
            TIFF::tiff
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
install(FILES third_party/libtiff/LICENSE.md
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/ScopeWriter/licenses/libtiff"
)
install(FILES third_party/zlib/LICENSE
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/ScopeWriter/licenses/zlib"
)
install(FILES third_party/zstd/LICENSE
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/ScopeWriter/licenses/zstd"
)
install(FILES third_party/crc32c/LICENSE
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/doc/ScopeWriter/licenses/crc32c"
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

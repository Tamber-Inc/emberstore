cmake_minimum_required(VERSION 3.25)

project(emberstore LANGUAGES CXX)

if (APPLE)
    enable_language(OBJCXX)
endif ()

include(cmake/CPM.cmake)

if (NOT TARGET eacp-core)
    CPMAddPackage(
            NAME eacp
            GITHUB_REPOSITORY eyalamirmusic/eacp
            GIT_TAG main)
endif ()

if (NOT TARGET Miro)
    CPMAddPackage(
            NAME Miro
            GITHUB_REPOSITORY eyalamirmusic/Miro
            GIT_TAG main)
endif ()

add_library(emberstore STATIC
        emberstore/AtomicFile.cpp)

# The filesystem backends (file watching, inter-process locks) are macOS and
# Windows only — a platform without one fails the build rather than silently
# doing nothing.
if (APPLE)
    target_sources(emberstore PRIVATE
            emberstore/FileWatcher-macOS.mm
            emberstore/FileLock-POSIX.cpp)
    target_link_libraries(emberstore PUBLIC
            "-framework CoreServices" "-framework Foundation")
elseif (WIN32)
    target_sources(emberstore PRIVATE
            emberstore/FileWatcher-Windows.cpp
            emberstore/FileLock-Windows.cpp)
else ()
    message(FATAL_ERROR "emberstore: no filesystem backend for this platform")
endif ()

target_include_directories(emberstore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(emberstore PUBLIC cxx_std_20)
target_link_libraries(emberstore PUBLIC eacp-core Miro)

if (PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(tests)
endif ()

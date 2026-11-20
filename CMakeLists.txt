cmake_minimum_required(VERSION 3.25)

project(emberstore LANGUAGES CXX)

if (APPLE)
    enable_language(OBJCXX)
endif ()

option(EMBERSTORE_ASAN "Build with AddressSanitizer" OFF)

if (EMBERSTORE_ASAN)
    if (MSVC)
        add_compile_options(/fsanitize=address)
    else ()
        add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address)
    endif ()
endif ()

# Warnings-as-errors for this repo's own targets only — dependencies are built
# with their own flags.
if (MSVC)
    set(EMBERSTORE_WARNINGS /W4 /WX)
else ()
    set(EMBERSTORE_WARNINGS -Wall -Werror)
endif ()

include(cmake/CPM.cmake)

# Pinned to the jamierpond fork's jp/calder-emberstore branch (Miro's develop
# plus the open PRs Calder needs), the same Miro Calder pins, until those PRs
# land on eyalamirmusic/Miro develop. Added before
# eacp, which pulls Miro main itself — CPM keeps whichever version is added
# first.
if (NOT TARGET Miro)
    CPMAddPackage(
            NAME Miro
            GITHUB_REPOSITORY jamierpond/Miro
            GIT_TAG 6aa7ecc44ab708ca0ae23b1aac9a79787e8cd544)
endif ()

# The binary format (Calder::toBinary, journals) lives in jamierpond/Calder.
if (NOT TARGET calder)
    CPMAddPackage(
            NAME Calder
            GITHUB_REPOSITORY jamierpond/Calder
            GIT_TAG 497f48dd1ae3f7fd1fe64f6cd7982c1af53a1874)
endif ()

if (NOT TARGET eacp-core)
    CPMAddPackage(
            NAME eacp
            GITHUB_REPOSITORY eyalamirmusic/eacp
            GIT_TAG 257563bee87e69bae41ecf29934c37f3eaec7e49)
endif ()

add_library(emberstore STATIC
        emberstore/AtomicFile.cpp)

# The file-watching backend is per-platform — FSEvents, ReadDirectoryChangesW,
# inotify — and a platform without one fails the build rather than silently
# doing nothing. (Inter-process locking now comes from eacp, which carries its
# own per-platform backend.)
if (APPLE)
    target_sources(emberstore PRIVATE
            emberstore/FileWatcher-macOS.mm)
    target_link_libraries(emberstore PUBLIC
            "-framework CoreServices" "-framework Foundation")
elseif (WIN32)
    target_sources(emberstore PRIVATE
            emberstore/FileWatcher-Windows.cpp)
elseif (LINUX)
    # inotify is in glibc; no extra library to link.
    target_sources(emberstore PRIVATE
            emberstore/FileWatcher-Linux.cpp)
else ()
    message(FATAL_ERROR "emberstore: no filesystem backend for this platform")
endif ()

target_include_directories(emberstore PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(emberstore PUBLIC cxx_std_20)
target_compile_options(emberstore PRIVATE ${EMBERSTORE_WARNINGS})
# eacp-network carries the interprocess lock (eacp::IPC::Lock) that Document.h
# now leans on; PUBLIC because that lock leaks through our public headers.
target_link_libraries(emberstore PUBLIC eacp-core eacp-network Miro calder)

if (PROJECT_IS_TOP_LEVEL)
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(Apps)
endif ()

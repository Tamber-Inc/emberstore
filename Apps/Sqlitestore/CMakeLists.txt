# SQLite comes in as the plain amalgamation — no upstream CMake, so the zip is
# download-only and the target is built here.
enable_language(C)

CPMAddPackage(
        NAME sqlite3
        URL https://www.sqlite.org/2026/sqlite-amalgamation-3520000.zip
        DOWNLOAD_ONLY YES)

if (sqlite3_ADDED AND NOT TARGET sqlite3)
    add_library(sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
    target_include_directories(sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
    # dlopen + pthreads live outside libc on older glibc.
    find_package(Threads REQUIRED)
    target_link_libraries(sqlite3 PUBLIC ${CMAKE_DL_LIBS} Threads::Threads)
endif ()

add_executable(sqlitestore Main.cpp)
target_link_libraries(sqlitestore PRIVATE emberstore sqlite3)
target_compile_options(sqlitestore PRIVATE ${EMBERSTORE_WARNINGS})

if (NOT TARGET NanoTest)
    CPMAddPackage(
            NAME NanoTest
            GITHUB_REPOSITORY eyalamirmusic/NanoTest
            GIT_TAG main)
endif ()

# Test entry point + helpers for real multi-process file contention. Link this
# instead of NanoTestMain: it provides main() and adds the --contest-file mode.
add_library(TestContest STATIC
        support/ContestFile.cpp
        support/ContestMain.cpp)

target_include_directories(TestContest PUBLIC support)
target_compile_features(TestContest PUBLIC cxx_std_20)
target_link_libraries(TestContest PUBLIC
        NanoTest
        emberstore)

# TestContest replaces NanoTestMain: it owns main() and lets this binary
# re-invoke itself as a real contending process (--contest-file).
add_executable(EmberstoreTests
        LocalDatabaseTests.cpp DatabaseTests.cpp DatabaseContentionTests.cpp
        CollectionPerfTests.cpp LiveDocumentTests.cpp AppDatabaseTests.cpp)
target_link_libraries(EmberstoreTests PRIVATE
        NanoTest TestContest emberstore)
nano_discover_tests(EmberstoreTests)

add_executable(FileWatcherTests FileWatcherTests.cpp)
target_link_libraries(FileWatcherTests PRIVATE
        NanoTest NanoTestMain emberstore)
nano_discover_tests(FileWatcherTests)

add_executable(SearchDemo SearchDemo.cpp)
target_link_libraries(SearchDemo PRIVATE emberstore)

foreach (target TestContest EmberstoreTests FileWatcherTests SearchDemo)
    target_compile_options(${target} PRIVATE ${EMBERSTORE_WARNINGS})
endforeach ()

add_executable(bytelayout Main.cpp ByteLayoutAnnotator.cpp)
target_link_libraries(bytelayout PRIVATE emberstore)
target_compile_options(bytelayout PRIVATE ${EMBERSTORE_WARNINGS})

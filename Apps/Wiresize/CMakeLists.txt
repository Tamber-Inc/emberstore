add_executable(wiresize Main.cpp)
target_link_libraries(wiresize PRIVATE emberstore)
target_compile_options(wiresize PRIVATE ${EMBERSTORE_WARNINGS})

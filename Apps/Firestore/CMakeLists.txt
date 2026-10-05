add_executable(firestore Main.cpp)
target_link_libraries(firestore PRIVATE emberstore)
target_compile_options(firestore PRIVATE ${EMBERSTORE_WARNINGS})

# ── All tests link against the core static library ─────────────────────
function(add_tv_test NAME)
    add_executable(${NAME} ${NAME}.cpp)
    target_include_directories(${NAME} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(${NAME} PRIVATE tv_core GTest::gtest_main pthread)
    add_test(NAME ${NAME} COMMAND ${NAME})
endfunction()

add_tv_test(test_models)
add_tv_test(test_retry)
add_tv_test(test_crypto)
add_tv_test(test_compression)
add_tv_test(test_chunker)

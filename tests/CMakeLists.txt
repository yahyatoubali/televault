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
add_tv_test(test_async_stress)
add_tv_test(test_m2_stress)

add_executable(m2_cross_helper m2_cross_helper.cpp)
target_include_directories(m2_cross_helper PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(m2_cross_helper PRIVATE tv_core pthread)

add_executable(mercury_tests order_book_test.cpp)
target_link_libraries(mercury_tests PRIVATE mercury GTest::gtest_main)
mercury_set_warnings(mercury_tests)

include(GoogleTest)
gtest_discover_tests(mercury_tests)

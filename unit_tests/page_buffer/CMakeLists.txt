enable_testing()
include(GoogleTest)

add_executable(
  test_page_buffer
  test_page_buffer.cpp
)
target_link_libraries(
  test_page_buffer
  GTest::gtest_main
)

add_test(
  NAME test_page_buffer
  COMMAND test_page_buffer
)

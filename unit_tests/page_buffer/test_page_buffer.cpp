#include <gtest/gtest.h>
#include "page_buffer.h"
#include "test_page_buffer_common.hpp"

// Demonstrate some basic assertions.
TEST (HelloTest, BasicAssertions)
{
  // Expect two strings not to be equal.
  EXPECT_STRNE ("hello", "world");
  // Expect equality.
  EXPECT_EQ (7 * 6, 42);
}

TEST (PageBufferTest, SampleTest)
{

}

int main (int argc, char **argv)
{
  ::testing::InitGoogleTest (&argc, argv);
  ::testing::AddGlobalTestEnvironment (new ServerEnv());
  ::testing::GTEST_FLAG (break_on_failure) = true;

  // TIP:
  // While on active development, oos_log level is set to DEBUG.
  // This makes the test output verbose.
  // We need to explicitly set it to INFO or higher level to make test output clean.
  // For debugging test failures, we can set it back to DEBUG or TRACE.
  return RUN_ALL_TESTS();
}


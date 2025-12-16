#include <gtest/gtest.h>
// #include "page_buffer.h"
#include "disk_manager.h"
#include "test_page_buffer_common.hpp"
#include "page_buffer.c"

// Demonstrate some basic assertions.
TEST (HelloTest, BasicAssertions)
{
  // Expect two strings not to be equal.
  EXPECT_STRNE ("hello", "world");
  // Expect equality.
  EXPECT_EQ (7 * 6, 42);
}

TEST (PageBufferTest, InitFinalize)
{
  // int rc = pgbuf_initialize();
  // EXPECT_EQ (rc, NO_ERROR);
  //
  // pgbuf_finalize();
}

TEST (PageBufferTest, HashVpid)
{
  VPID vpid1 = { 1, 100 };
  VPID vpid2 = { 2, 200 };
  unsigned int htsize = 1024;

  unsigned int hash1 = pgbuf_hash_vpid (&vpid1, htsize);
  unsigned int hash2 = pgbuf_hash_vpid (&vpid2, htsize);

  EXPECT_NE (hash1, hash2);
}

TEST (PageBufferTest, CompareVpid)
{
  VPID vpid1 = { 1, 100 };
  VPID vpid2 = { 1, 100 };
  VPID vpid3 = { 2, 200 };

  int cmp1 = pgbuf_compare_vpid (&vpid1, &vpid2);
  int cmp2 = pgbuf_compare_vpid (&vpid1, &vpid3);

  EXPECT_EQ (cmp1, 0);
  EXPECT_NE (cmp2, 0);
}

TEST (PageBufferTest, IsValidPage)
{
  DISK_ISVALID is_valid = pgbuf_is_valid_page (thread_p, &vpid_Null_vpid, true);
  EXPECT_EQ (is_valid, DISK_INVALID);
}


TEST (PageBufferTest, FixDebugOldPage)
{
  // auto page_ptr = pgbuf_fix (thread_p, &vpid_Null_vpid, PAGE_FETCH_MODE::OLD_PAGE, PGBUF_LATCH_MODE::PGBUF_LATCH_READ,
  // 	     PGBUF_LATCH_CONDITION::PGBUF_UNCONDITIONAL_LATCH);
}

TEST (PageBufferTest, FixNewPage)
{
  // auto page_ptr = pgbuf_fix (thread_p, &vpid_Null_vpid, PAGE_FETCH_MODE::NEW_PAGE, PGBUF_LATCH_MODE::PGBUF_LATCH_READ,
  // 	     PGBUF_LATCH_CONDITION::PGBUF_UNCONDITIONAL_LATCH);
}

TEST (PageBufferTest, FixOldPageIfInBuffer)
{
  auto page_ptr = pgbuf_fix (thread_p, &vpid_Null_vpid, PAGE_FETCH_MODE::OLD_PAGE_IF_IN_BUFFER,
  	     PGBUF_LATCH_MODE::PGBUF_LATCH_READ, PGBUF_LATCH_CONDITION::PGBUF_UNCONDITIONAL_LATCH);
  EXPECT_EQ (page_ptr, nullptr);
}

int main (int argc, char **argv)
{
  ::testing::InitGoogleTest (&argc, argv);
  ::testing::AddGlobalTestEnvironment (new ServerEnv());
  ::testing::GTEST_FLAG (break_on_failure) = true;

  return RUN_ALL_TESTS();
}


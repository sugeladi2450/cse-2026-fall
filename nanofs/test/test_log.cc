#include <nanofs/log.h>
#include <gtest/gtest.h>
#include <cstdio>
#include <fstream>
#include <sstream>

using namespace nanofs;

TEST(ParseLevel, ValidStrings) {
  ASSERT_TRUE(log::parse_level("debug"));
  EXPECT_EQ(*log::parse_level("debug"), log::Level::Debug);
  ASSERT_TRUE(log::parse_level("info"));
  EXPECT_EQ(*log::parse_level("info"), log::Level::Info);
  ASSERT_TRUE(log::parse_level("warn"));
  EXPECT_EQ(*log::parse_level("warn"), log::Level::Warn);
  ASSERT_TRUE(log::parse_level("error"));
  EXPECT_EQ(*log::parse_level("error"), log::Level::Error);
}

TEST(ParseLevel, InvalidStringIsError) {
  auto r = log::parse_level("verbose");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error(), Error::Invalid);
}

TEST(Enabled, ThresholdFiltering) {
  ASSERT_TRUE(log::init(log::Level::Warn));
  EXPECT_FALSE(log::enabled(log::Level::Debug));
  EXPECT_FALSE(log::enabled(log::Level::Info));
  EXPECT_TRUE(log::enabled(log::Level::Warn));
  EXPECT_TRUE(log::enabled(log::Level::Error));
}

TEST(Write, FileSinkRespectsThreshold) {
  const std::string path = "/tmp/nanofs_test_log.txt";
  std::remove(path.c_str());
  ASSERT_TRUE(log::init(log::Level::Warn, path));

  NANOFS_LOG_INFO("should not appear");
  NANOFS_LOG_WARN("should appear");

  std::ifstream in(path);
  std::stringstream buf;
  buf << in.rdbuf();
  const std::string contents = buf.str();

  EXPECT_EQ(contents.find("should not appear"), std::string::npos);
  EXPECT_NE(contents.find("[WARN] should appear"), std::string::npos);

  std::remove(path.c_str());
  ASSERT_TRUE(log::init(log::Level::Info));  // reset to stderr for any test run after this one
}

TEST(FormatLine, PlainHasNoEscapes) {
  EXPECT_EQ(log::format_line(log::Level::Warn, "slow rpc", false),
            "[WARN] slow rpc\n");
}

TEST(FormatLine, ColorWrapsTagOnly) {
  // The yellow color code wraps [WARN], with the reset after ']' and before
  // the message; the message itself is not colored.
  EXPECT_EQ(log::format_line(log::Level::Warn, "slow rpc", true),
            "\x1b[33m[WARN]\x1b[0m slow rpc\n");
  EXPECT_EQ(log::format_line(log::Level::Error, "disk full", true),
            "\x1b[1;31m[ERROR]\x1b[0m disk full\n");
}

TEST(ErrorToString, CoversEveryVariant) {
  EXPECT_EQ(to_string(Error::NotFound), "NotFound");
  EXPECT_EQ(to_string(Error::NoSpace), "NoSpace");
  EXPECT_EQ(to_string(Error::Exists), "Exists");
  EXPECT_EQ(to_string(Error::NotDir), "NotDir");
  EXPECT_EQ(to_string(Error::IsDir), "IsDir");
  EXPECT_EQ(to_string(Error::Invalid), "Invalid");
  EXPECT_EQ(to_string(Error::IoError), "IoError");
}

#include "markettwin/common/hash.hpp"

#include <gtest/gtest.h>
#include <string>

TEST(Sha256Test, StandardVectors) {
  EXPECT_EQ(markettwin::sha256(""),
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  EXPECT_EQ(markettwin::sha256("abc"),
            "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  EXPECT_EQ(markettwin::sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
  EXPECT_EQ(markettwin::sha256(std::string(1'000'000, 'a')),
            "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST(Sha256Test, IncrementalDigestDoesNotMutateState) {
  markettwin::Sha256 hash;
  hash.update("a");
  EXPECT_EQ(hash.digest(), markettwin::sha256("a"));
  hash.update("b");
  hash.update("c");
  EXPECT_EQ(hash.digest(), markettwin::sha256("abc"));
  EXPECT_EQ(hash.digest(), hash.digest());
  for (std::size_t size = 55; size <= 65; ++size) {
    markettwin::Sha256 split;
    const std::string input(size, static_cast<char>(0xff));
    split.update(std::string_view{input}.substr(0, 31));
    split.update(std::string_view{input}.substr(31));
    EXPECT_EQ(split.digest(), markettwin::sha256(input));
  }
}

#include "wisp/media/path.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace wisp;

TEST(MediaPath, AcceptsNestedUtf8) {
    const std::string p = "music/周杰伦/晴天.mp3";
    auto r = media::normalize(p);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value(), p);
}

TEST(MediaPath, RejectsEachForbiddenForm) {
    const std::string bad[] = {"",
                               "/music/a",
                               "music\\a",
                               "music//a",
                               "music/./a",
                               "music/../a",
                               ".index.json",
                               "music/.a.mp3",
                               std::string("music/a\x01"),
                               "music/",
                               std::string(256, 'a')};
    for (const auto& p : bad) {
        auto r = media::normalize(p);
        ASSERT_FALSE(r) << "accepted: " << p;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << p;
    }
    EXPECT_TRUE(media::normalize(std::string(255, 'a')));
}

TEST(MediaPath, EmptyDirMeansRootForList) {
    EXPECT_FALSE(media::normalize(""));
    auto r = media::normalize_dir("");
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value(), "");
    EXPECT_TRUE(media::normalize_dir("music/a"));
    EXPECT_FALSE(media::normalize_dir("../x"));
}

TEST(MediaPath, WritesStayInsideTheThreeFolders) {
    EXPECT_FALSE(media::normalize_for_write("x.txt"));
    EXPECT_FALSE(media::normalize_for_write("music"));
    EXPECT_FALSE(media::normalize_for_write("other/a.txt"));
    EXPECT_TRUE(media::normalize_for_write("document/n.txt"));
    EXPECT_TRUE(media::normalize_for_write("download/x/y.mp3"));
}

TEST(MediaPath, TrackExtensionIsCaseInsensitive) {
    EXPECT_TRUE(media::is_track_name("a.MP3"));
    EXPECT_TRUE(media::is_track_name("b.wav"));
    EXPECT_FALSE(media::is_track_name("c.flac"));
    EXPECT_FALSE(media::is_track_name("mp3"));
}

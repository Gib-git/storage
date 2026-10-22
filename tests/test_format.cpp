#include "check.hpp"

#include "storage/core/category.hpp"
#include "storage/core/format.hpp"

using namespace storage;

TEST_CASE("humanSize uses binary units") {
    CHECK(humanSize(0) == "0 B");
    CHECK(humanSize(1023) == "1023 B");
    CHECK(humanSize(1024) == "1.0 KB");
    CHECK(humanSize(1536) == "1.5 KB");
    CHECK(humanSize(500ULL * 1024 * 1024) == "500 MB");
    CHECK(humanSize(3ULL * 1024 * 1024 * 1024 * 1024) == "3.0 TB");
}

TEST_CASE("withThousands groups digits") {
    CHECK(withThousands(0) == "0");
    CHECK(withThousands(999) == "999");
    CHECK(withThousands(1000) == "1,000");
    CHECK(withThousands(1234567) == "1,234,567");
}

TEST_CASE("categorize maps extensions case-insensitively") {
    CHECK(categorize("IMG_0001.JPG") == Category::Image);
    CHECK(categorize("movie.mkv") == Category::Video);
    CHECK(categorize("song.flac") == Category::Audio);
    CHECK(categorize("report.pdf") == Category::Document);
    CHECK(categorize("backup.tar.gz") == Category::Archive);
    CHECK(categorize("main.cpp") == Category::Code);
    CHECK(categorize(".bashrc") == Category::Other);
    CHECK(categorize("README") == Category::Other);
    CHECK(categorize("trailingdot.") == Category::Other);
}

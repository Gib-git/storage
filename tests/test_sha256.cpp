#include <cstdio>
#include <string>

#include "check.hpp"
#include "storage/core/sha256.hpp"

using storage::Sha256;

TEST_CASE("sha256 matches NIST vectors") {
    std::printf("      (sha256 implementation: %s)\n", Sha256::implementation());
    CHECK(Sha256::hexOf("", 0) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::hexOf("abc", 3) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(Sha256::hexOf(two.data(), two.size()) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    const std::string million(1000000, 'a');
    CHECK(Sha256::hexOf(million.data(), million.size()) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("sha256 streaming equals one-shot") {
    std::string data;
    for (int i = 0; i < 10000; ++i) data.push_back(static_cast<char>(i * 31));
    Sha256 h;
    size_t pos = 0, step = 1;
    while (pos < data.size()) {
        const size_t n = std::min(step, data.size() - pos);
        h.update(data.data() + pos, n);
        pos += n;
        step = step * 3 % 97 + 1;  // odd chunk sizes across block boundaries
    }
    CHECK(Sha256::hex(h.finish()) == Sha256::hexOf(data.data(), data.size()));
}

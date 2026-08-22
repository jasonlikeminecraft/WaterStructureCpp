#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <vector>
#include <algorithm>

// Small dependency-free MD5 implementation used only to generate the TIBI
// fixture.  The production reader has its own bounded implementation; keeping
// this helper in the test tree avoids making the test suite depend on a
// platform crypto provider (BCrypt/OpenSSL).
inline std::array<std::uint8_t, 16> fixture_md5_portable(
    std::span<const std::uint8_t> input)
{
    static constexpr std::array<std::uint32_t, 64> shifts{
        7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
        5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
        4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
        6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
    };
    static constexpr std::array<std::uint32_t, 64> constants{
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,
        0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
        0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x6b901122,0xfd987193,0xa679438e,0x49b40821,
        0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,
        0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,
        0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
        0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
        0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
        0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,
        0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,
        0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
        0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    const auto padded_size = (input.size() + 9 + 63) & ~std::size_t{63};
    std::vector<std::uint8_t> padded(padded_size, 0);
    std::copy(input.begin(), input.end(), padded.begin());
    padded[input.size()] = 0x80;
    const auto bits = static_cast<std::uint64_t>(input.size()) * 8;
    for (std::size_t index = 0; index < 8; ++index) {
        padded[padded_size - 8 + index] = static_cast<std::uint8_t>(bits >> (index * 8));
    }
    std::uint32_t h0 = 0x67452301, h1 = 0xefcdab89;
    std::uint32_t h2 = 0x98badcfe, h3 = 0x10325476;
    for (std::size_t offset = 0; offset < padded.size(); offset += 64) {
        std::array<std::uint32_t, 16> words{};
        for (std::size_t word = 0; word < words.size(); ++word) {
            const auto base = offset + word * 4;
            words[word] = static_cast<std::uint32_t>(padded[base]) |
                (static_cast<std::uint32_t>(padded[base + 1]) << 8) |
                (static_cast<std::uint32_t>(padded[base + 2]) << 16) |
                (static_cast<std::uint32_t>(padded[base + 3]) << 24);
        }
        auto a = h0, b = h1, c = h2, d = h3;
        for (std::uint32_t index = 0; index < 64; ++index) {
            std::uint32_t function = 0, word = 0;
            if (index < 16) { function = (b & c) | (~b & d); word = index; }
            else if (index < 32) { function = (d & b) | (~d & c); word = (5 * index + 1) % 16; }
            else if (index < 48) { function = b ^ c ^ d; word = (3 * index + 5) % 16; }
            else { function = c ^ (b | ~d); word = (7 * index) % 16; }
            const auto old_d = d;
            d = c;
            c = b;
            b += std::rotl(a + function + constants[index] + words[word],
                static_cast<int>(shifts[index]));
            a = old_d;
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }
    std::array<std::uint8_t, 16> result{};
    const std::array hash{h0, h1, h2, h3};
    for (std::size_t word = 0; word < hash.size(); ++word) {
        for (std::size_t byte = 0; byte < 4; ++byte) {
            result[word * 4 + byte] = static_cast<std::uint8_t>(hash[word] >> (byte * 8));
        }
    }
    return result;
}

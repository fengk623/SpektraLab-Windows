#include "io/linear_prophoto_icc.hpp"

#include <cmath>
#include <cstring>

namespace spk::io {
namespace {

void put32(std::vector<std::uint8_t>& b, std::uint32_t v) {
    b.push_back(std::uint8_t(v >> 24)); b.push_back(std::uint8_t(v >> 16));
    b.push_back(std::uint8_t(v >> 8)); b.push_back(std::uint8_t(v));
}
void put16(std::vector<std::uint8_t>& b, std::uint16_t v) { b.push_back(std::uint8_t(v >> 8)); b.push_back(std::uint8_t(v)); }
void put_s15f16(std::vector<std::uint8_t>& b, double v) { put32(b, std::uint32_t(std::int32_t(std::lround(v * 65536.0)))); }
void put_sig(std::vector<std::uint8_t>& b, const char* sig) { b.insert(b.end(), sig, sig + 4); }

// multiLocalizedUnicodeType with one en-US record.
std::vector<std::uint8_t> mluc(const std::string& ascii) {
    std::vector<std::uint8_t> d;
    put_sig(d, "mluc"); put32(d, 0);
    put32(d, 1); put32(d, 12);
    put_sig(d, "enUS");
    put32(d, std::uint32_t(ascii.size() * 2)); put32(d, 28);
    for (char c : ascii) put16(d, std::uint16_t(static_cast<unsigned char>(c)));
    return d;
}

std::vector<std::uint8_t> xyz(double x, double y, double z) {
    std::vector<std::uint8_t> d;
    put_sig(d, "XYZ "); put32(d, 0);
    put_s15f16(d, x); put_s15f16(d, y); put_s15f16(d, z);
    return d;
}

std::vector<std::uint8_t> build() {
    struct Tag { const char* sig; std::vector<std::uint8_t> data; };
    const auto& m = kLinearProphotoToXyzD50;
    // curveType with no entries: the identity response (ICC.1:2010 10.5).
    std::vector<std::uint8_t> linear;
    put_sig(linear, "curv"); put32(linear, 0); put32(linear, 0);
    const std::vector<Tag> tags = {
        {"desc", mluc(kLinearProphotoIccDescription)},
        {"cprt", mluc("No copyright, use freely")},
        {"wtpt", xyz(0.9642, 1.0, 0.8249)},
        {"rXYZ", xyz(m[0], m[3], m[6])},
        {"gXYZ", xyz(m[1], m[4], m[7])},
        {"bXYZ", xyz(m[2], m[5], m[8])},
        {"rTRC", linear},
        {"gTRC", linear},   // the three share one element
        {"bTRC", linear},
    };
    std::vector<std::uint8_t> body;
    std::vector<std::array<std::uint32_t, 2>> places;
    const std::uint32_t table = 128 + 4 + std::uint32_t(tags.size()) * 12;
    std::uint32_t shared_curve = 0;
    for (const Tag& t : tags) {
        const bool later_curve = std::strcmp(t.sig, "gTRC") == 0 || std::strcmp(t.sig, "bTRC") == 0;
        if (later_curve) { places.push_back({shared_curve, std::uint32_t(t.data.size())}); continue; }
        const std::uint32_t at = table + std::uint32_t(body.size());
        if (std::strcmp(t.sig, "rTRC") == 0) shared_curve = at;
        body.insert(body.end(), t.data.begin(), t.data.end());
        while (body.size() % 4) body.push_back(0);
        places.push_back({at, std::uint32_t(t.data.size())});
    }

    std::vector<std::uint8_t> p;
    put32(p, table + std::uint32_t(body.size()));   // profile size
    put32(p, 0);                                    // preferred CMM
    put32(p, 0x04300000);                           // version 4.3
    put_sig(p, "mntr"); put_sig(p, "RGB "); put_sig(p, "XYZ ");
    // A fixed creation date (2026-11-12 00:00:00 UTC): the bytes, and with
    // them the Profile ID, do not depend on when or where this was built.
    put16(p, 2026); put16(p, 11); put16(p, 12); put16(p, 0); put16(p, 0); put16(p, 0);
    put_sig(p, "acsp");
    put32(p, 0); put32(p, 0); put32(p, 0); put32(p, 0);   // platform, flags, manufacturer, model
    put32(p, 0); put32(p, 0);                             // attributes
    put32(p, 0);                                          // rendering intent: perceptual
    put_s15f16(p, 0.9642); put_s15f16(p, 1.0); put_s15f16(p, 0.8249);   // PCS illuminant (D50)
    put32(p, 0);                                          // creator
    p.resize(128, 0);                                     // Profile ID (below) and reserved
    put32(p, std::uint32_t(tags.size()));
    for (std::size_t i = 0; i < tags.size(); ++i) {
        put_sig(p, tags[i].sig);
        put32(p, places[i][0]);
        put32(p, places[i][1]);
    }
    p.insert(p.end(), body.begin(), body.end());

    // ICC.1:2010 7.2.18: MD5 of the profile with the flags (44..47), the
    // rendering intent (64..67) and the ID itself (84..99) set to zero. All
    // three are zero here already, so the profile as built is the message.
    const auto id = md5(p.data(), p.size());
    std::memcpy(&p[84], id.data(), id.size());
    return p;
}

}  // namespace

const std::vector<std::uint8_t>& linear_prophoto_icc() {
    static const std::vector<std::uint8_t> profile = build();
    return profile;
}

std::string linear_prophoto_icc_id() {
    const auto& p = linear_prophoto_icc();
    static const char hex[] = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 84; i < 100; ++i) { out.push_back(hex[p[i] >> 4]); out.push_back(hex[p[i] & 15]); }
    return out;
}

bool is_linear_prophoto_icc(const std::uint8_t* data, std::size_t size) {
    const auto& p = linear_prophoto_icc();
    return data && size == p.size() && std::memcmp(data, p.data(), size) == 0;
}

std::array<std::uint8_t, 16> md5(const std::uint8_t* data, std::size_t size) {
    static const std::uint32_t k[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int shift[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                  5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                  4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                  6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::vector<std::uint8_t> message(data, data + size);
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    const std::uint64_t bits = std::uint64_t(size) * 8;
    for (int i = 0; i < 8; ++i) message.push_back(std::uint8_t(bits >> (8 * i)));

    std::uint32_t a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
    for (std::size_t block = 0; block < message.size(); block += 64) {
        std::uint32_t w[16];
        for (int i = 0; i < 16; ++i)
            w[i] = std::uint32_t(message[block + i * 4]) | std::uint32_t(message[block + i * 4 + 1]) << 8 |
                   std::uint32_t(message[block + i * 4 + 2]) << 16 | std::uint32_t(message[block + i * 4 + 3]) << 24;
        std::uint32_t a = a0, b = b0, c = c0, d = d0;
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            f += a + k[i] + w[g];
            a = d; d = c; c = b;
            b += (f << shift[i]) | (f >> (32 - shift[i]));
        }
        a0 += a; b0 += b; c0 += c; d0 += d;
    }
    std::array<std::uint8_t, 16> out{};
    const std::uint32_t words[4] = {a0, b0, c0, d0};
    for (int i = 0; i < 16; ++i) out[i] = std::uint8_t(words[i / 4] >> (8 * (i % 4)));
    return out;
}

}  // namespace spk::io

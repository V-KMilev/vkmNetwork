// BitWriter and BitReader: what is written reads back, to the bit, and the bytes are fixed.

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "vkm_network/bit_stream.h"

#include "harness/check.h"

namespace {

using Vkm::Network::BitReader;
using Vkm::Network::BitWriter;

uint32_t rawOf(float value) {
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    return raw;
}

float floatOf(uint32_t raw) {
    float value = 0.0f;
    std::memcpy(&value, &raw, sizeof(value));
    return value;
}

} // namespace

TEST("every width from 1 to 32 reads back what was written, at every bit offset") {
    int wrong = 0;
    std::string first;
    for (uint32_t offset = 0; offset < 8; ++offset) {
        for (uint32_t width = 1; width <= 32; ++width) {
            for (const uint32_t pattern : {0xFFFFFFFFu, 0xA5A5A5A5u, 0x5A5A5A5Au, 0x80000001u, 0u}) {
                const uint32_t value = width == 32 ? pattern : pattern & ((1u << width) - 1u);

                std::vector<uint8_t> bytes;
                BitWriter            out(bytes, 16);
                out.bits(0x55u, offset);
                out.bits(pattern, width);
                out.bits(0x2u, 2);
                out.finish();

                BitReader in(bytes.data(), bytes.size());
                in.bits(offset);
                const uint32_t read   = in.bits(width);
                const uint32_t marker = in.bits(2);
                const bool right = read == value && marker == 0x2u && !in.failed() && !out.overflowed()
                    && out.bitCount() == offset + width + 2 && bytes.size() == (offset + width + 2 + 7) / 8;
                if (!right && wrong++ == 0) {
                    first = "offset " + std::to_string(offset) + ", width " + std::to_string(width)
                        + ": wrote " + std::to_string(value) + ", read " + std::to_string(read);
                }
            }
        }
    }
    CHECK(wrong == 0);
    CHECK(first == "");
}

TEST("a u64 goes high word first, and an f32 as its exact bits: signed zero, infinities and a NaN's payload") {
    std::vector<uint8_t> bytes;
    BitWriter            out(bytes, 64);
    out.u64(0x0123456789ABCDEFull);
    out.finish();
    CHECK(bytes == std::vector<uint8_t>{0x67, 0x45, 0x23, 0x01, 0xEF, 0xCD, 0xAB, 0x89});
    BitReader wide(bytes.data(), bytes.size());
    CHECK(wide.u64() == 0x0123456789ABCDEFull);

    using Limits = std::numeric_limits<float>;
    const float values[] = {
        -0.0f, Limits::infinity(), -Limits::infinity(), floatOf(0x7FC12345u), floatOf(0xFF801234u),
        Limits::denorm_min(), 1.5f,
    };
    BitWriter floats(bytes, 64);
    floats.bits(0x5u, 3);
    for (const float value : values) floats.f32(value);
    floats.finish();

    BitReader in(bytes.data(), bytes.size());
    in.bits(3);
    for (const float value : values) CHECK(rawOf(in.f32()) == rawOf(value));
    CHECK(!in.failed());
}

TEST("a fixed sequence of writes gives fixed bytes") {
    std::vector<uint8_t> bytes;
    BitWriter            out(bytes, 32);
    out.bits(0x5u, 3);
    out.u8(0xAB);
    out.boolean(true);
    out.u16(0x1234);
    out.u32(0xDEADBEEFu);
    out.f32(1.0f);
    out.bits(0x3u, 2);
    out.u64(0x0102030405060708ull);
    out.finish();
    // Each field's bits lowest first, packed from each byte's lowest bit.
    const std::vector<uint8_t> packed = {
        0x5D, 0x4D, 0x23, 0xF1, 0xEE, 0xDB, 0xEA, 0x0D, 0x00, 0x00,
        0xF8, 0x33, 0xC1, 0x80, 0x40, 0x00, 0xC2, 0x81, 0x41, 0x01,
    };
    CHECK(bytes == packed);
}

TEST("a write past the capacity is refused and the overflow stays said, and a read past the end fails and gives zero") {
    std::vector<uint8_t> bytes;
    BitWriter            out(bytes, 2);
    out.u8(0x11);
    CHECK(!out.overflowed());
    out.bits(0x1FFu, 9);
    CHECK(out.overflowed());
    out.u8(0x22);
    CHECK(out.overflowed());
    const uint8_t spliced[] = {0xFF};
    out.append(spliced, 1);
    out.finish();
    CHECK(bytes == std::vector<uint8_t>{0x11, 0x22});
    CHECK(out.bitCount() == 16u);

    BitReader in(bytes.data(), bytes.size());
    CHECK(in.u16() == 0x2211u);
    CHECK(!in.failed());
    CHECK(in.bits(1) == 0u);
    CHECK(in.failed());
    CHECK(in.u8() == 0u);
    CHECK(in.failed());

    // Past the end in the middle of a field: nothing of it is read.
    BitReader part(bytes.data(), bytes.size());
    part.bits(12);
    CHECK(part.bits(5) == 0u);
    CHECK(part.failed());
}

TEST("bits appended from another stream, whole bytes and a part of one, read back in place") {
    std::vector<uint8_t> encoded;
    BitWriter            first(encoded, 8);
    first.u8(0xC3);
    first.bits(0x15u, 5);
    first.finish();

    std::vector<uint8_t> bytes;
    BitWriter            out(bytes, 8);
    out.bits(0x6u, 3);
    out.append(encoded.data(), 13);
    out.bits(0x1u, 2);
    out.finish();
    CHECK(out.bitCount() == 18u);

    BitReader in(bytes.data(), bytes.size());
    CHECK(in.bits(3) == 0x6u);
    CHECK(in.u8() == 0xC3u);
    CHECK(in.bits(5) == 0x15u);
    CHECK(in.bits(2) == 0x1u);
    CHECK(!in.failed());

    // Appending past the capacity is refused like any write.
    std::vector<uint8_t> small;
    BitWriter            tight(small, 1);
    tight.append(encoded.data(), 13);
    CHECK(tight.overflowed());
    CHECK(tight.bitCount() == 0u);
}

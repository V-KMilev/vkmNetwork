// The coders: a float as a signed or fixed-point code, and a rotation as its smallest three.

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "vkm_network/bit_stream.h"
#include "vkm_network/quantize.h"

#include "harness/check.h"
#include "harness/scratch.h"

namespace {

using namespace Vkm::Network;

constexpr float ROTATION_RANGE = 0.70710678f;  ///< A component other than the largest: +/- 1/sqrt(2).

uint32_t middleCode(uint32_t width) { return (1u << (width - 1u)) - 1u; }

/// a * b + c, where the target can fuse it: x86's baseline has no fused multiply-add to fuse with.
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
__attribute__((target("fma")))
#endif
float multiplyAdd(float a, float b, float c) {
    return a * b + c;
}

/// The angle between two rotations, in radians, worked out in double.
double angleBetween(const glm::quat& a, const glm::quat& b) {
    const glm::dquat delta = glm::conjugate(glm::dquat(a)) * glm::dquat(b);
    return 2.0 * std::atan2(glm::length(glm::dvec3(delta.x, delta.y, delta.z)), std::abs(delta.w));
}

glm::quat roundTrip(const glm::quat& q, uint32_t width) {
    std::vector<uint8_t> bytes;
    BitWriter            out(bytes, 16);
    writeRotation(out, q, width);
    out.finish();
    BitReader in(bytes.data(), bytes.size());
    return readRotation(in, width);
}

} // namespace

TEST("a signed code is zero exactly at its middle at every width, the range's ends at the end codes, and NaN is zero") {
    const float range = 3.0f;
    for (uint32_t width = 2; width <= 32; ++width) {
        const uint32_t middle = middleCode(width);
        CHECK(toSigned(0.0f, range, width) == middle);
        CHECK(toSigned(-0.0f, range, width) == middle);
        CHECK(fromSigned(middle, range, width) == 0.0f);
        CHECK(toSigned(std::numeric_limits<float>::quiet_NaN(), range, width) == middle);

        CHECK(toSigned(range, range, width) == 2u * middle);
        CHECK(toSigned(-range, range, width) == 0u);
        CHECK(fromSigned(2u * middle, range, width) == range);
        CHECK(fromSigned(0u, range, width) == -range);

        // Past the range, clamped to its ends.
        CHECK(toSigned(range * 4.0f, range, width) == 2u * middle);
        CHECK(toSigned(-range * 4.0f, range, width) == 0u);
        CHECK(toSigned(std::numeric_limits<float>::infinity(), range, width) == 2u * middle);
    }
}

TEST("a signed code reads back within half a step, and a corrupt one inside the range") {
    const float range = 2.0f;
    for (uint32_t width = 2; width <= 16; ++width) {
        const float step = range / static_cast<float>(middleCode(width));
        float worst = 0.0f;
        for (int i = -100; i <= 100; ++i) {
            const float value = range * static_cast<float>(i) / 100.0f;
            const float back  = fromSigned(toSigned(value, range, width), range, width);
            worst = std::max(worst, std::abs(back - value));
        }
        CHECK(worst <= step * 0.5f + 1e-6f);

        // The top code is one toSigned never writes; a damaged packet still can.
        const uint32_t top = width == 32 ? 0xFFFFFFFFu : (1u << width) - 1u;
        CHECK(fromSigned(top, range, width) <= range);
        CHECK(fromSigned(top, range, width) >= -range);
    }
    CHECK(fromSigned(0xFFFFFFFFu, range, 32) <= range);
    CHECK(fromSigned(0xFFFFFFFFu, range, 32) >= -range);
}

TEST("a fixed-point code rounds to the nearest step, is clamped at both ends, and NaN is the first code") {
    const float low  = -4.0f;
    const float step = 0.25f;
    CHECK(toFixed(low, low, step, 8) == 0u);
    CHECK(toFixed(low + 2.4f * step, low, step, 8) == 2u);
    CHECK(toFixed(low + 2.6f * step, low, step, 8) == 3u);
    CHECK(toFixed(low - 10.0f, low, step, 8) == 0u);
    CHECK(toFixed(-std::numeric_limits<float>::infinity(), low, step, 8) == 0u);
    CHECK(toFixed(low + 1000.0f, low, step, 8) == 255u);
    CHECK(toFixed(std::numeric_limits<float>::infinity(), low, step, 8) == 255u);
    CHECK(toFixed(1e30f, low, step, 32) == 0xFFFFFFFFu);
    CHECK(toFixed(std::numeric_limits<float>::quiet_NaN(), low, step, 8) == 0u);
    CHECK(toFixed(std::numeric_limits<float>::quiet_NaN(), low, step, 32) == 0u);
    CHECK(toFixed(0.49999997f, 0.0f, 1.0f, 8) == 0u);
    CHECK(fromFixed(0u, low, step) == low);
    CHECK(fromFixed(3u, low, step) == low + 0.75f);
    CHECK(fromFixed(255u, low, step) == low + 63.75f);
}

TEST("fixedBits is the fewest bits whose codes reach across the span") {
    for (const float step : {1.0f, 0.25f, 0.001f}) {
        for (const float span : {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 100.0f, 1024.0f}) {
            const uint32_t bits = fixedBits(span, step);
            const float    top  = fromFixed(toFixed(span, 0.0f, step, bits), 0.0f, step);
            CHECK_NEAR(top, span, step * 0.5f);
            // One fewer does not reach.
            if (bits > 1) {
                CHECK(fromFixed(toFixed(span, 0.0f, step, bits - 1), 0.0f, step) < span - step * 0.5f);
            }
        }
    }
    CHECK(fixedBits(1e30f, 1e-30f) == 32u);
}

TEST("a rotation comes back within the angle its width allows, the identity exactly, and q and -q alike") {
    for (const uint32_t width : {8u, 11u, 16u}) {
        // Each of the three is off by at most half a step, sqrt(3) halves together; the largest, made
        // from them and never under a half, at most doubles that on the sphere, and a rotation turns
        // twice its quaternion's angle: 2 sqrt(3) steps.
        const double allowed = 2.0 * std::sqrt(3.0) * ROTATION_RANGE / middleCode(width) * 1.01 + 1e-6;
        double worst = 0.0;
        for (int i = 0; i < 4000; ++i) {
            const float     t = static_cast<float>(i);
            const glm::quat q = glm::normalize(glm::quat(
                std::sin(t * 1.3f), std::cos(t * 0.7f), std::sin(t * 2.9f + 1.0f), std::cos(t * 0.31f + 2.0f)
            ));
            worst = std::max(worst, angleBetween(q, roundTrip(q, width)));
        }
        CHECK(worst <= allowed);
    }

    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    CHECK(roundTrip(identity, 11) == identity);

    const glm::quat q = glm::normalize(glm::quat(-0.3f, 0.5f, -0.7f, 0.2f));
    std::vector<uint8_t> plus;
    std::vector<uint8_t> minus;
    BitWriter            a(plus, 16);
    BitWriter            b(minus, 16);
    writeRotation(a, q, 11);
    writeRotation(b, -q, 11);
    a.finish();
    b.finish();
    CHECK(plus == minus);
}

TEST("a rotation read from any code is a unit quaternion") {
    float worst = 0.0f;
    for (uint32_t seed = 0; seed < 3000; ++seed) {
        // Patterns of bits spread over the codes, the impossible ones included: three components at
        // their ends.
        const uint32_t word = seed * 2654435761u;
        const uint8_t raw[] = {
            static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word >> 16),
            static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(seed), 0xFF,
        };
        BitReader in(raw, sizeof(raw));
        const glm::quat q = readRotation(in, 11);
        worst = std::max(worst, std::abs(glm::length(q) - 1.0f));
    }
    CHECK(worst <= 1e-5f);

    const uint8_t extremes[] = {0xFC, 0xFF, 0xFF, 0xFF, 0xFF};
    BitReader     in(extremes, sizeof(extremes));
    CHECK_NEAR(glm::length(readRotation(in, 11)), 1.0f, 1e-5f);
}

TEST("fixed values and rotations at fixed widths encode to fixed bytes") {
    std::vector<uint8_t> bytes;
    BitWriter            out(bytes, 64);
    // A stick's axes, 8 bits across +/-1.
    for (const float axis : {-1.0f, -0.5f, 0.0f, 0.25f, 1.0f}) out.bits(toSigned(axis, 1.0f, 8), 8);
    // Positions, 20 bits from -512 at a millimeter.
    const uint32_t positionBits = fixedBits(1024.0f, 0.001f);
    CHECK(positionBits == 20u);
    for (const float position : {-512.0f, 0.0f, 1.25f, 256.0f}) {
        out.bits(toFixed(position, -512.0f, 0.001f, positionBits), positionBits);
    }
    // Velocities, 16 bits across +/-200.
    for (const float velocity : {-200.0f, 0.0f, 3.5f, 200.0f}) out.bits(toSigned(velocity, 200.0f, 16), 16);
    // Rotations, 11 bits a component.
    writeRotation(out, glm::quat(1.0f, 0.0f, 0.0f, 0.0f), 11);
    writeRotation(out, glm::quat(0.5f, 0.5f, 0.5f, 0.5f), 11);
    writeRotation(out, glm::quat(0.8f, 0.6f, 0.0f, 0.0f), 11);
    writeRotation(out, glm::quat(-0.8f, 0.6f, 0.0f, 0.0f), 11);
    out.finish();

    // The codes: axes 0 63 127 159 254; positions 0 512000 513250 768000; velocities 0 32767 33340 65534;
    // rotations (largest, then the other three) 3 1023 1023 1023, 0 1746 1746 1746, 3 1891 1023 1023,
    // 3 155 1023 1023.
    const std::vector<uint8_t> expected = {
        0x00, 0x3F, 0x7F, 0x9F, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x7D, 0xE2, 0xD4,
        0x07, 0x80, 0xBB, 0x00, 0x00, 0xFF, 0x7F, 0x3C, 0x82, 0xFE, 0xFF, 0xFF,
        0xEF, 0x7F, 0xFF, 0x43, 0xDA, 0xD2, 0x96, 0xF6, 0x63, 0xFF, 0xDF, 0xFF,
        0xDE, 0xC4, 0xFF, 0xFE, 0x07,
    };
    CHECK(bytes == expected);
}

TEST("a fixed-point code is the nearest step past 2^23 steps too, where a float holds no half") {
    int wrong = 0;
    for (uint32_t code = (1u << 23) + 1u; code < (1u << 23) + 64u; ++code) {
        if (toFixed(static_cast<float>(code), 0.0f, 1.0f, 24) != code) ++wrong;
    }
    CHECK(wrong == 0);
}

TEST("a consumer's arithmetic is not fused, as the coders' is not") {
#if defined(__GNUC__) && !defined(__clang__) && !defined(__OPTIMIZE__)
    Vkm::Test::skip("GCC fuses nothing unoptimized");
#endif
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
    if (!__builtin_cpu_supports("fma")) Vkm::Test::skip("this processor has no fused multiply-add");
#endif
    // a * b rounds to 1 + 2^-11 before c is added, unfused; fused, the 2^-24 it lost survives.
    volatile float a = 1.0f + 0x1p-12f;
    volatile float b = 1.0f + 0x1p-12f;
    volatile float c = -(1.0f + 0x1p-11f);
    CHECK(multiplyAdd(a, b, c) == 0.0f);
}

TEST("with the library's asserts on, a broken precondition stops the program") {
    if (!VKM_TEST_LIBRARY_ASSERTS) Vkm::Test::skip("the library's asserts are off in this build");
    // A signed field is 2 to 32 bits.
    const Vkm::Test::Ended ended = Vkm::Test::inChild([] { (void)toSigned(1.0f, 1.0f, 33); });
    CHECK(ended == Vkm::Test::Ended::Stopped);
}

#pragma once

#include <cstdint>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "vkm_network/bit_stream.h"

namespace Vkm::Network {

/**
 * @brief The code for @p value in [-@p range, @p range], @p width bits wide,
 *        with zero exact.
 *
 * Zero must be exact: off by half a step, a rotation tilts, a body at rest
 * drifts and a stick creeps. Past the range a value is clamped; NaN is zero.
 *
 * @param value The value to quantize.
 * @param range Magnitude spanned either side of zero.
 * @param width Bits, 2 to 32.
 * @return The code, ready for BitWriter::bits with @p width.
 */
uint32_t toSigned(float value, float range, uint32_t width);

/// Read back what toSigned wrote, bounded to the same range.
float fromSigned(uint32_t raw, float range, uint32_t width);

/**
 * @brief The code for @p value on a grid of @p step from @p low, @p width bits wide.
 *
 * Rounded to the nearest step; below @p low is the first code and past the
 * last the width holds is the last.
 *
 * @param value The value to quantize.
 * @param low   The value the first code stands for.
 * @param step  How far apart two codes are.
 * @param width Bits, 1 to 32.
 * @return The code, ready for BitWriter::bits with @p width.
 */
uint32_t toFixed(float value, float low, float step, uint32_t width);

/// The value toFixed's code @p raw stands for. Not bounded: a damaged packet can carry any code.
float fromFixed(uint32_t raw, float low, float step);

/**
 * @brief Bits a fixed-point field needs to cover @p span at @p step.
 *
 * @param span  The width of the range, from its lowest value to its highest.
 * @param step  How far apart two codes are.
 * @return The fewest bits, at most 32, whose codes reach across @p span.
 */
constexpr uint32_t fixedBits(float span, float step) {
    uint32_t bits = 1;
    float range = 2.0f;
    const float need = span / step;
    while (range < need && bits < 32u) {
        range *= 2.0f;
        ++bits;
    }
    return bits;
}

/**
 * @brief Write a rotation as its three smallest components, each @p componentBits wide.
 *
 * The largest is recovered from the other three and a two-bit index; it is
 * made positive, since q and -q are the same rotation.
 *
 * @param out           The packet.
 * @param value         The rotation; normalized before it is written.
 * @param componentBits Bits each of the three takes, 2 to 32.
 */
void writeRotation(BitWriter& out, const glm::quat& value, uint32_t componentBits);

/// Read back what writeRotation wrote at @p componentBits, normalized.
glm::quat readRotation(BitReader& in, uint32_t componentBits);

} // namespace Vkm::Network

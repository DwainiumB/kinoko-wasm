#pragma once

#include <egg/math/Vector.hh>

/// @file The two vector helpers the game's item code shares. They differ from
/// EGG::Vector3f::normalise in rounding: the item code (and the Dolphin recordings) depend on the
/// exact operation order.
namespace Kinoko::Item {

/// @addr{0x807872C0}
/// @brief Writes the unit vector of `in` to `out` when `in` is not zero: `in * frsqrt(|in|^2)`, the
/// square taken as PSVECMag does it (z*z fused onto x*x, then y*y added).
inline bool NormaliseInto(EGG::Vector3f &out, const EGG::Vector3f &in) {
    f32 sq = in.ps_squareMag();
    if (sq > 0.0f) {
        out = in * EGG::Mathf::frsqrt(sq);
        return true;
    }

    return false;
}

/// @addr{0x80793F04}
/// @brief Normalises `in` into `out` and returns its length: the length through PSVECMag
/// (0x8019AC68), the unit vector as `in * (1 / length)` with a true division. `out` is left alone
/// when the length is 0.
inline f32 NormaliseLength(EGG::Vector3f &out, const EGG::Vector3f &in) {
    f32 mag = in.ps_length();
    if (mag > 0.0f) {
        out = in * (1.0f / mag);
    }

    return mag;
}

} // namespace Kinoko::Item

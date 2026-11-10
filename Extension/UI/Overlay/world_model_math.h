#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <utility>
#include <vector>

// The plain arithmetic of a world model (world_model.h), apart from the GPU, so it can be tested.
namespace dingosdk::world_model {

// ---- the game's near distance -------------------------------------------------------------------------
// The game's depth is reversed and infinite: a pixel's depth is near / (its distance along the view).
// Its near distance is in no place ReSkate knows, and a model drawn into the world has to compare its own
// distance with the game's, so it is measured. A world model is drawn where something of the game's lies
// just behind it (a board's tread on the deck's underside): there depth * our distance is the near
// distance, give or take how far behind the game's surface lies. Where the game's surface is in front
// (the model hidden behind something) the product is larger; at an edge, where the pixel shows what is
// far beyond, smaller (the shader leaves out pixels whose neighbours' depths differ). So the products are
// counted in bins, and the lowest bin with a good share of them is the near distance.
inline constexpr std::size_t near_bins = 256;
inline constexpr float near_lowest = 1e-3f, near_highest = 10.0f;
// Each bin is this many times the one before: 4 decades over 256 bins, 3.7 %.
inline float near_ratio() noexcept { return std::pow(near_highest / near_lowest, 1.0f / near_bins); }
inline float near_bin_low(std::size_t bin) noexcept { return near_lowest * std::pow(near_ratio(), static_cast<float>(bin)); }
// The shader keeps, beside each bin's count, the sum of how far into the bin each product lay, as a
// fraction of the bin's lower edge times sum_scale: the mean of a bin is then exact to well under 1 %.
inline constexpr float sum_scale = 65536.0f;

struct NearSample {
    std::array<std::uint32_t, near_bins> counts{}, sums{};
};
// One frame's measure, or nothing when it has too few pixels to say (`least` of them in the cluster).
inline std::optional<float> near_of(const NearSample& sample, std::uint32_t least = 48) {
    std::uint64_t total{};
    for (const auto c : sample.counts) total += c;
    if (total < least) return std::nullopt;
    const auto wanted = std::max<std::uint64_t>(least, total / 20); // 5 % of what was counted
    for (std::size_t b = 0; b + 1 < near_bins; ++b) {
        const std::uint64_t pair = std::uint64_t{sample.counts[b]} + sample.counts[b + 1];
        if (pair < wanted) continue;
        double weighted{}, count{};
        for (std::size_t k = b; k <= b + 1; ++k) {
            if (!sample.counts[k]) continue;
            const double mean = near_bin_low(k) * (1.0 + sample.sums[k] / (sum_scale * static_cast<double>(sample.counts[k])));
            weighted += mean * sample.counts[k];
            count += sample.counts[k];
        }
        return static_cast<float>(weighted / count);
    }
    return std::nullopt;
}
// The measure kept across frames: the middle of the last ones, once three agree on something.
class NearEstimate {
public:
    void add(float value) {
        if (!(value > near_lowest && value < near_highest)) return;
        recent_.push_back(value);
        if (recent_.size() > 15) recent_.pop_front();
    }
    std::optional<float> value() const {
        if (recent_.size() < 3) return std::nullopt;
        std::vector<float> sorted(recent_.begin(), recent_.end());
        std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
        return sorted[sorted.size() / 2];
    }
    void reset() { recent_.clear(); }

private:
    std::deque<float> recent_;
};

// ---- placing a model ---------------------------------------------------------------------------------
using Vec3 = std::array<float, 3>;
using Quat = std::array<float, 4>; // x, y, z, w
// The model's own space to the world: three rows of a 4x4 whose last row is 0 0 0 1, so that
// world = m * (x, y, z, 1): turned by `rotation`, made `scale` times bigger, then moved to `position`.
inline std::array<float, 12> placement(const Vec3& position, Quat q, float scale) noexcept {
    const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(length > 1e-6f)) q = {0, 0, 0, 1};
    else for (auto& v : q) v /= length;
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    const std::array<float, 9> r{1 - 2 * (y * y + z * z), 2 * (x * y - z * w),     2 * (x * z + y * w),
                                 2 * (x * y + z * w),     1 - 2 * (x * x + z * z), 2 * (y * z - x * w),
                                 2 * (x * z - y * w),     2 * (y * z + x * w),     1 - 2 * (x * x + y * y)};
    return {r[0] * scale, r[1] * scale, r[2] * scale, position[0],
            r[3] * scale, r[4] * scale, r[5] * scale, position[1],
            r[6] * scale, r[7] * scale, r[8] * scale, position[2]};
}
// a then b in a's frame (a * b).
inline Quat multiply(const Quat& a, const Quat& b) noexcept {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
inline Vec3 rotate(const Quat& q, const Vec3& v) noexcept {
    const Vec3 u{q[0], q[1], q[2]};
    const float w = q[3];
    const Vec3 t{2 * (u[1] * v[2] - u[2] * v[1]), 2 * (u[2] * v[0] - u[0] * v[2]), 2 * (u[0] * v[1] - u[1] * v[0])};
    return {v[0] + w * t[0] + (u[1] * t[2] - u[2] * t[1]), v[1] + w * t[1] + (u[2] * t[0] - u[0] * t[2]),
            v[2] + w * t[2] + (u[0] * t[1] - u[1] * t[0])};
}
// A placement within a frame (the board's deck) put in the world: the frame's turn then the model's,
// and the model's place turned by the frame and moved to it.
inline std::pair<Vec3, Quat> within(const Vec3& frame_at, const Quat& frame_turn, const Vec3& at, const Quat& turn) noexcept {
    const auto moved = rotate(frame_turn, at);
    return {{frame_at[0] + moved[0], frame_at[1] + moved[1], frame_at[2] + moved[2]}, multiply(frame_turn, turn)};
}
inline Vec3 apply(const std::array<float, 12>& m, const Vec3& p) noexcept {
    return {m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3], m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
            m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11]};
}
}

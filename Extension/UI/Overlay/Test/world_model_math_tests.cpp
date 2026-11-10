#include "Extension/UI/Overlay/world_model_math.h"

#include <cmath>
#include <cstdio>

// A world model's measure of the game's near distance and its placement
// (Extension/UI/Overlay/world_model_math.h): products counted as the shader counts them.
namespace {
using namespace dingosdk::world_model;
int failures = 0;

void expect(bool condition, const char* what) {
    if (condition) return;
    ++failures;
    std::printf("FAIL: %s\n", what);
}
bool near(float a, float b, float within) { return std::fabs(a - b) <= within; }

// As world_model.hlsl counts a product.
void count(NearSample& sample, float product, std::uint32_t times = 1) {
    const float at = std::log(product / near_lowest) / std::log(near_ratio());
    if (!(at >= 0) || at >= near_bins) return;
    const auto bin = static_cast<std::size_t>(at);
    sample.counts[bin] += times;
    sample.sums[bin] += static_cast<std::uint32_t>((product / near_bin_low(bin) - 1.0f) * sum_scale) * times;
}

void the_near_distance_is_found_under_hidden_and_edge_pixels() {
    NearSample sample;
    // The tread over the deck: just under 0.1 (the deck lies a few millimetres behind the tread).
    count(sample, 0.0996f, 400);
    count(sample, 0.0999f, 300);
    // Hidden behind a rail: larger products.
    count(sample, 0.18f, 900);
    // A few edge pixels that slipped through: far smaller.
    count(sample, 0.004f, 5);
    const auto found = near_of(sample);
    expect(found && near(*found, 0.0997f, 0.0005f), "the cluster under the hidden pixels, not the stray edges");
}

void too_few_pixels_say_nothing() {
    NearSample sample;
    count(sample, 0.1f, 10);
    expect(!near_of(sample), "ten pixels are not enough");
    expect(!near_of(NearSample{}), "nothing drawn says nothing");
}

void the_estimate_holds_the_middle_of_recent_frames() {
    NearEstimate estimate;
    estimate.add(0.1f);
    estimate.add(0.1f);
    expect(!estimate.value(), "not before three frames");
    estimate.add(0.5f); // a frame with the model all hidden
    expect(estimate.value() && near(*estimate.value(), 0.1f, 1e-6f), "one odd frame does not move it");
    estimate.add(0.0f);
    estimate.add(100.0f);
    expect(estimate.value() && near(*estimate.value(), 0.1f, 1e-6f), "values out of range are not kept");
    estimate.reset();
    expect(!estimate.value(), "reset forgets");
}

void a_model_is_turned_scaled_and_moved() {
    // A quarter turn about y: the model's x goes to the world's -z.
    const float s = std::sqrt(0.5f);
    const auto m = placement({1, 2, 3}, {0, s, 0, s}, 2.0f);
    const auto p = apply(m, {1, 0, 0});
    expect(near(p[0], 1, 1e-5f) && near(p[1], 2, 1e-5f) && near(p[2], 1, 1e-5f), "turned, doubled and moved");
    const auto still = placement({0, 0, 0}, {0, 0, 0, 0}, 1.0f);
    const auto q = apply(still, {0.5f, 0.25f, -1});
    expect(near(q[0], 0.5f, 1e-6f) && near(q[1], 0.25f, 1e-6f) && near(q[2], -1, 1e-6f), "no rotation is the identity");
}
} // namespace

int main() {
    the_near_distance_is_found_under_hidden_and_edge_pixels();
    too_few_pixels_say_nothing();
    the_estimate_holds_the_middle_of_recent_frames();
    a_model_is_turned_scaled_and_moved();
    if (failures) {
        std::printf("%d world model check(s) failed\n", failures);
        return 1;
    }
    std::printf("world model checks passed\n");
    return 0;
}

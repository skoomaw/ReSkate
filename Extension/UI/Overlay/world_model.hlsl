// A world model (world_model.cpp, reskate.world_model) drawn into the game's lit, not yet tone-mapped
// picture of the world. t0 is a copy of that picture (the light around the model is read from it), t1
// the game's own depth (reversed and infinite: depth = near / distance along the view). Two pixel
// shaders: ps_measure counts depth * distance where the model lies, from which the game's near distance
// is found (world_model_math.h); ps_main draws it, where nothing of the game's is in front of it.

cbuffer frame : register(b0) {
    float4 model0, model1, model2; // the model's space to the world: rows of a 3x4
    float4 right_focal;            // the camera's right (world), and its focal length in pixels
    float4 up_width;               // its up, and the picture's width
    float4 back_height;            // its back (it looks along -back), and the picture's height
    float4 eye_near;               // where it is, and the game's near distance (0: not known yet)
    float4 options;                // how far behind the game's surface still counts as in front of it (m),
                                   // and the same per metre of distance; how much a glance adds; unused
};
cbuffer material : register(b1) {
    float4 colour_opacity;
};
Texture2D<float4> scene : register(t0);
Texture2D<float> depth : register(t1);
RWByteAddressBuffer counted : register(u0); // 256 counts, then 256 sums (world_model_math.h)

static const float near_lowest = 1e-3;
static const float log_ratio = 0.035977; // ln(1e4) / 256: each bin 3.7 % above the last
static const float sum_scale = 65536.0;
// Our own depth, for the model's own parts in front of each other; the game's is compared by hand.
static const float own_near = 0.02, own_far = 1000.0;

struct In {
    float3 position : POSITION;
    float3 normal : NORMAL;
};
struct Out {
    float4 position : SV_Position;
    float3 world : WORLD;
    float3 normal : NORMAL;
    float ahead : AHEAD; // distance along the view
};

Out vs_main(In v) {
    Out o;
    const float4 p = float4(v.position, 1);
    o.world = float3(dot(model0, p), dot(model1, p), dot(model2, p));
    const float3 n = float3(dot(model0.xyz, v.normal), dot(model1.xyz, v.normal), dot(model2.xyz, v.normal));
    o.normal = n;
    const float3 d = o.world - eye_near.xyz;
    const float x = dot(d, right_focal.xyz), y = dot(d, up_width.xyz), z = -dot(d, back_height.xyz);
    o.ahead = z;
    const float a = own_far / (own_far - own_near);
    o.position = float4(x * right_focal.w * 2.0 / up_width.w, y * right_focal.w * 2.0 / back_height.w, z * a - own_near * a, z);
    return o;
}

float depth_at(int2 p) {
    p = clamp(p, int2(0, 0), int2((int)up_width.w, (int)back_height.w) - 1);
    return depth.Load(int3(p, 0));
}

void ps_measure(Out i) {
    if (i.ahead <= own_near) return;
    const int2 p = int2(i.position.xy);
    const float d = depth_at(p);
    if (!(d > 0.0 && d < 1.0)) return;
    // Not at an edge of the game's: the neighbours lie at much the same depth.
    const float l = depth_at(p - int2(2, 0)), r = depth_at(p + int2(2, 0)), u = depth_at(p - int2(0, 2)), b = depth_at(p + int2(0, 2));
    const float spread = max(max(abs(l - d), abs(r - d)), max(abs(u - d), abs(b - d)));
    if (spread > 0.03 * d) return;
    const float product = d * i.ahead;
    const float at = log(product / near_lowest) / log_ratio;
    if (!(at >= 0.0) || at >= 256.0) return;
    const uint bin = (uint)at;
    const float low = near_lowest * exp(bin * log_ratio);
    counted.InterlockedAdd(bin * 4, 1);
    counted.InterlockedAdd(1024 + bin * 4, (uint)((product / low - 1.0) * sum_scale));
}

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float4 ps_main(Out i) : SV_Target {
    const int2 p = int2(i.position.xy);
    const float d = depth_at(p);
    // Behind something of the game's (a little behind its surface still counts as in front: the deck the
    // tread lies on is a few millimetres behind it).
    if (d > 0.0 && eye_near.w > 0.0) {
        const float game = eye_near.w / d;
        if (i.ahead > game + options.x + options.y * i.ahead) discard;
    }
    // The light where it is: the brightest of the game's pixels around it.
    float light = luminance(scene.Load(int3(p, 0)).rgb);
    [unroll] for (int k = 0; k < 8; ++k) {
        const float angle = k * 0.785398;
        const int2 q = clamp(p + int2((int)(cos(angle) * 14.0), (int)(sin(angle) * 14.0)), int2(0, 0), int2((int)up_width.w, (int)back_height.w) - 1);
        light = max(light, luminance(scene.Load(int3(q, 0)).rgb));
    }
    light = clamp(light, 0.03, 64.0);
    const float3 n = normalize(i.normal);
    const float3 v = normalize(eye_near.xyz - i.world);
    const float glance = pow(1.0 - abs(dot(n, v)), 3.0) * options.z;
    const float3 colour = colour_opacity.rgb * light * (0.85 + 0.6 * glance);
    const float opacity = saturate(colour_opacity.a + (1.0 - colour_opacity.a) * glance * 0.6);
    return float4(colour, opacity);
}

// A cover over the world's upward-facing surfaces (world_cover.cpp, reskate.effects.cover), drawn over
// the game's lit, not yet tone-mapped picture of the world. One full-screen triangle; t0 is a copy of
// that picture, t1 the game's own depth for it (reversed: 1 near, 0 far).

cbuffer constants : register(b0) {
    float4 right_focal;   // the camera's right (world), and its focal length in pixels
    float4 up_width;      // its up, and the picture's width
    float4 back_height;   // its back (it looks along -back), and the picture's height
    float4 eye_near;      // where it is, and the near distance the reversed depth is over
    float4 colour_amount; // the cover's colour, and how much of a covered surface it hides
    float4 shape;         // facing, softness, brightness, breakup
    float4 extra;         // sparkle, unused, unused, unused
    float4 clear_info;    // how many places are kept clear (0 to 4)
    float4 clears[4];     // each: its foot in the world, and its radius
    float4 clear_heights; // each one's height above its foot
};
Texture2D<float4> scene : register(t0);
Texture2D<float> depth : register(t1);

struct Vertex { float4 position : SV_Position; };

Vertex vs_main(uint id : SV_VertexID) {
    Vertex v;
    const float2 uv = float2((id << 1) & 2, id & 2);
    v.position = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}

float depth_at(int2 p) {
    p = clamp(p, int2(0, 0), int2(up_width.w, back_height.w) - 1);
    return depth.Load(int3(p, 0));
}

// The point the pixel shows, in the world. Reversed, infinite depth: distance = near / depth.
float3 world_at(int2 p, float d, float near_distance) {
    const float a = (p.x + 0.5 - up_width.w * 0.5) / right_focal.w;
    const float b = (back_height.w * 0.5 - (p.y + 0.5)) / right_focal.w;
    const float3 ray = right_focal.xyz * a + up_width.xyz * b - back_height.xyz;
    return eye_near.xyz + ray * (near_distance / max(d, 1e-7));
}

// The near distance the game's depth is over, measured where the first place kept clear stands: its
// distance along the view, times the depth drawn where its foot shows on screen (depth = near /
// distance). 0 when it cannot be measured (no place kept clear, or its foot is off screen or behind).
// The camera's own near plane is not known, and without it distances are only right in proportion.
float measured_near() {
    if (clear_info.x < 1) return 0;
    const float3 to = clears[0].xyz - eye_near.xyz;
    const float distance = dot(to, -back_height.xyz);
    if (distance < 0.3) return 0;
    const float2 at = float2(up_width.w * 0.5 + dot(to, right_focal.xyz) * right_focal.w / distance,
                             back_height.w * 0.5 - dot(to, up_width.xyz) * right_focal.w / distance);
    if (any(at < 0) || at.x >= up_width.w || at.y >= back_height.w) return 0;
    const float there = depth_at(int2(at));
    return there > 0 ? distance * there : 0;
}

float hash(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}
float noise(float2 p) {
    const float2 i = floor(p), f = frac(p);
    const float2 u = f * f * (3 - 2 * f);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), u.x), lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

float4 ps_main(Vertex input) : SV_Target {
    const int2 p = int2(input.position.xy);
    const float4 under = scene.Load(int3(p, 0));
    const float d = depth_at(p);
    // The sky, and anything at the near plane (the camera inside something).
    if (!(d > 0.0 && d < 1.0)) discard;
    const float near_measured = measured_near();
    const float near_distance = near_measured > 0 ? near_measured : eye_near.w;
    const float3 here = world_at(p, d, near_distance);
    // The surface's direction from its neighbours: on each axis the one nearer in depth, so an edge
    // against something further away does not bend it.
    const float dl = depth_at(p - int2(1, 0)), dr = depth_at(p + int2(1, 0));
    const float du = depth_at(p - int2(0, 1)), dd = depth_at(p + int2(0, 1));
    const bool use_right = abs(dr - d) < abs(dl - d), use_down = abs(dd - d) < abs(du - d);
    const int2 px = use_right ? p + int2(1, 0) : p - int2(1, 0);
    const int2 py = use_down ? p + int2(0, 1) : p - int2(0, 1);
    float3 across = world_at(px, use_right ? dr : dl, near_distance) - here;
    float3 along = world_at(py, use_down ? dd : du, near_distance) - here;
    if (!use_right) across = -across;
    if (!use_down) along = -along;
    float3 normal = cross(along, across);
    const float length_squared = dot(normal, normal);
    if (!(length_squared > 1e-20)) discard;
    normal *= rsqrt(length_squared);
    // Facing the camera: the side the pixel is seen from.
    if (dot(normal, eye_near.xyz - here) < 0) normal = -normal;

    // Patchy where it thins: large, slow noise over the ground, in the world, so it stays put.
    const float patch = noise(here.xz * 0.45) * 0.65 + noise(here.xz * 1.9) * 0.35;
    const float facing = shape.x + (patch - 0.5) * shape.w * 0.6;
    const float softness = max(shape.y, 0.01);
    float cover = smoothstep(facing - softness, facing + softness, normal.y) * colour_amount.w;
    // Only on surfaces flat across their neighbours. A plane's reversed depth changes evenly across the
    // screen, so its second difference is nothing; leaves, wires and distant edges are not flat, their
    // direction comes out differently each frame, and the cover drawn after the game's anti-aliasing
    // flickered on them (2026-11-10).
    const float bend = max(abs(dl + dr - 2 * d), abs(du + dd - 2 * d)) / d;
    cover *= 1 - smoothstep(0.0015, 0.006, bend);
    // The places kept clear (the skater): upright cylinders over their feet, softened at the edge. Only
    // when distances are measured, or the cylinder would be in the wrong place.
    if (near_measured > 0) {
        [unroll] for (int c = 0; c < 4; ++c) {
            if (c >= (int)clear_info.x) break;
            const float3 from = here - clears[c].xyz;
            const float radius = clears[c].w;
            const float inside = (1 - smoothstep(radius * 0.8, radius, length(from.xz))) * step(-0.3, from.y) * step(from.y, clear_heights[c]);
            cover *= 1 - inside;
        }
    }
    if (cover <= 0.002) discard;

    // Lit as what it covers is lit, without its colour: so a shadow on the ground is a shadow on the
    // snow, and the game's exposure holds for both. Not from the pixel alone, or the ground's own
    // pattern (paving joints, cracks) shows through as grey: the brightest of a few close samples on the
    // same surface (as deep as this pixel), so dark joints vanish under the snow. Close, and the same
    // surface only: a wide ring of samples copied the skater and shadow edges onto the snow around them
    // eight times over (2026-11-10).
    const int2 size = int2(up_width.w, back_height.w);
    const float reach = max(back_height.w * 0.006, 2.0);
    float light = luminance(under.rgb);
    float base = light;
    [unroll] for (int i = 0; i < 12; ++i) {
        const float angle = i * 0.5235988 + (i & 1) * 0.26;
        const float2 way = float2(cos(angle), sin(angle)) * reach * ((i & 1) ? 1.0 : 0.5);
        const int2 at = clamp(p + int2(way), int2(0, 0), size - 1);
        const float there = depth_at(at);
        if (abs(there - d) > d * 0.02) continue; // another surface: the skater, a pole, a kerb's edge
        light = max(light, luminance(scene.Load(int3(at, 0)).rgb));
    }
    light = lerp(light, base, 0.1);
    float3 lit = colour_amount.rgb * light * shape.z;
    if (extra.x > 0) {
        const float glint = hash(floor(here.xz * 38.0) + floor(here.y * 38.0));
        lit += light * extra.x * 6.0 * step(0.996, glint);
    }
    return float4(lerp(under.rgb, lit, cover), under.a);
}

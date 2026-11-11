// A cover over the world's upward-facing surfaces (world_cover.cpp, reskate.effects.cover), drawn into
// the finished scene just before the game puts it on the screen with its UI, and snow falling in front
// of it. One full-screen triangle; t0 is a copy of that picture, t1 the game's own depth for it
// (reversed: 1 near, 0 far: our copy of it, which ps_depth takes), t2 the cover as it was last frame (how much, and how far away), which
// this pass writes again to its second target, and t3 the trails through the cover (a buffer).

cbuffer constants : register(b0) {
    float4 right_focal;   // the camera's right (world), and its focal length in pixels
    float4 up_width;      // its up, and the picture's width
    float4 back_height;   // its back (it looks along -back), and the picture's height
    float4 eye_near;      // where it is, and the near distance the reversed depth is over when not measured
    float4 colour_amount; // the cover's colour, and how much of a covered surface it hides
    float4 shape;         // facing, softness, brightness, breakup
    float4 extra;         // sparkle, seconds (for the falling snow), how much snow falls (0 to 1), how fast (1 the usual)
    float4 clear_info;    // how many places are kept clear (0 to 3), then each one's height above its foot
    float4 clears[3];     // each: its foot in the world, and its radius
    float4 last_right;    // last frame's camera (as this frame's above), and whether last frame's cover is there
    float4 last_up;       // ..., and how much of this frame's cover goes into what is shown (the rest is last frame's)
    float4 last_back;
    float4 last_eye;
};
Texture2D<float4> scene : register(t0);
Texture2D<float> depth : register(t1);
Texture2D<float2> last_cover : register(t2);
// [0] = (points, the box they lie in: min x, min z, max x), [1] = (max z, min y, max y, -), then two a
// point, oldest first: (where, width), (how strong it still is, 0 to 1, -, -, -). After the most points
// (world_cover.cpp's most_trail), the cameras ps_measure tries: each its right, up, back and eye.
StructuredBuffer<float4> trail : register(t3);
#define MOST_TRAIL 256
#define CANDIDATES 1
SamplerState linear_clamp : register(s0);

struct Vertex { float4 position : SV_Position; };
struct Output {
    float4 colour : SV_Target0;
    float2 cover : SV_Target1; // how much cover, and the distance along the view it is at
};

// Our copy of the game's depth (world_cover.cpp, snapshot_depth): t1 is the game's own here.
float ps_depth(Vertex input) : SV_Target { return depth.Load(int3(int2(input.position.xy), 0)); }

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

// The ray through a pixel, in the world (not of unit length: one along the view).
float3 ray_of(float2 p) {
    const float a = (p.x + 0.5 - up_width.w * 0.5) / right_focal.w;
    const float b = (back_height.w * 0.5 - (p.y + 0.5)) / right_focal.w;
    return right_focal.xyz * a + up_width.xyz * b - back_height.xyz;
}

// The point the pixel shows, in the world. Reversed, infinite depth: distance = near / depth.
float3 world_at(int2 p, float d, float near_distance) {
    return eye_near.xyz + ray_of(p) * (near_distance / max(d, 1e-7));
}

// The near distance the game's depth is over, measured on the first place kept clear (the skater): its
// distance along the view, times the depth drawn where it shows on screen (depth = near / distance).
// Where its foot shows first; where its middle shows (a metre up, its surface a little nearer) when the
// foot is off screen, as in a close view. 0 when neither shows. The camera's own near plane is not
// known, and without it distances are only right in proportion.
float measure_with(float3 right, float3 up, float3 back, float3 eye, float3 target, float nearer) {
    const float3 to = target - eye;
    const float along_view = dot(to, -back);
    if (along_view < 0.3) return 0;
    const float2 at = float2(up_width.w * 0.5 + dot(to, right) * right_focal.w / along_view,
                             back_height.w * 0.5 - dot(to, up) * right_focal.w / along_view);
    if (any(at < 0) || at.x >= up_width.w || at.y >= back_height.w) return 0;
    const float there = depth_at(int2(at));
    return there > 0 ? (along_view - nearer) * there : 0;
}
float measure_at(float3 target, float nearer) {
    return measure_with(right_focal.xyz, up_width.xyz, back_height.xyz, eye_near.xyz, target, nearer);
}
float measured_near() {
    if (clear_info.x < 1) return 0;
    const float foot = measure_at(clears[0].xyz, 0);
    return foot > 0 ? foot : measure_at(clears[0].xyz + float3(0, 1, 0), 0.12);
}

// Measurements for world_cover.cpp to hold the near distance by (a 32 x 1 target, read back by the CPU):
// each column a point up the skater's own body, from a little under their place to over their head, a
// few centimetres either side of its middle. Each is the near distance that point says the depth is
// over (its surface taken as 0.1 m nearer than its middle), or 0 off screen. Those that show the skater
// agree; those that miss them show something further away. The CPU keeps the biggest group that agrees.
// The distance measured at one pixel jumped as the foot, board or shoe showed there, and the trails
// and the place kept clear slid about with it; points on the ground round the skater were no better:
// the skater's place is not on the ground, and they measured the air (2026-11-11).
float ps_measure(Vertex input) : SV_Target {
    const int2 p = int2(input.position.xy);
    if (clear_info.x < 1 || p.y >= CANDIDATES) return 0;
    const int at = 2 + MOST_TRAIL * 2 + p.y * 4;
    const uint k = (uint)p.x;
    const float side = (k < 16u ? -0.06 : 0.06);
    const float3 target = clears[0].xyz + float3(0, -0.4 + (k % 16u) * 0.13, 0) + trail[at].xyz * side;
    return measure_with(trail[at].xyz, trail[at + 1].xyz, trail[at + 2].xyz, trail[at + 3].xyz, target, 0.1);
}

float hash(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}
float3 hash3(float3 p) {
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.xxy + p.yxx) * p.zyx);
}
float noise(float2 p) {
    const float2 i = floor(p), f = frac(p);
    const float2 u = f * f * (3 - 2 * f);
    return lerp(lerp(hash(i), hash(i + float2(1, 0)), u.x), lerp(hash(i + float2(0, 1)), hash(i + float2(1, 1)), u.x), u.y);
}

float luminance(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// The middle brightness of 16 samples of the picture round p, on the same plane as `here` (within
// `flat` metres of it along `normal`), reaching up to `reach` pixels, turned by `spin`: `fallback`
// when fewer than four are on the plane.
float median_light(int2 p, float3 here, float3 normal, float near_distance, float reach, float flat, float spin, float fallback) {
    const int2 size = int2(up_width.w, back_height.w);
    float v[16];
    int found = 0;
    [unroll] for (int i = 0; i < 16; ++i) {
        const float angle = spin + i * 2.3999632; // golden angle: spread evenly, no two in line
        const float radius = reach * sqrt((i + 0.5) / 16.0);
        const int2 at = clamp(p + int2(float2(cos(angle), sin(angle)) * radius), int2(0, 0), size - 1);
        const float there = depth_at(at);
        const bool same = there > 0 && abs(dot(world_at(at, there, near_distance) - here, normal)) <= flat;
        v[i] = same ? luminance(scene.Load(int3(at, 0)).rgb) : -1.0;
        found += same ? 1 : 0;
    }
    if (found < 4) return fallback;
    // Sorted (16 is few): those off the plane (-1) first, then the middle of the rest.
    [unroll] for (int a = 1; a < 16; ++a) {
        [unroll] for (int b = a; b > 0; --b) {
            const float lo = min(v[b - 1], v[b]), hi = max(v[b - 1], v[b]);
            v[b - 1] = lo;
            v[b] = hi;
        }
    }
    return v[16 - found + found / 2];
}

// A tread over the underside of the local skater's deck (world_cover.cpp: five float4s after the
// cameras): the deck's rotation, its place (w: 1 when on), the tread's colour and opacity, its half
// width, half length, height from the deck's bone and how thick a slice of the deck counts as its
// underside, then the ridges' spacing and width. Only where the surface faces the deck's down.
float3 tread_over(float3 shown, float3 here, float3 normal) {
    const int at = 2 + MOST_TRAIL * 2 + CANDIDATES * 4;
    const float4 place = trail[at + 1];
    if (place.w < 0.5) return shown;
    const float4 q = trail[at];
    const float4 look = trail[at + 2];
    const float4 size = trail[at + 3];
    const float4 ridges = trail[at + 4];
    // Into the deck's frame: the conjugate rotation of the offset.
    const float3 v = here - place.xyz;
    const float3 u = -q.xyz;
    const float3 t = 2 * cross(u, v);
    const float3 local = v + q.w * t + cross(u, t);
    const float3 tn = 2 * cross(u, normal);
    const float3 local_normal = normal + q.w * tn + cross(u, tn);
    if (local_normal.y > -0.5 || abs(local.y - size.z) > size.w) return shown;
    // A rounded patch: straight along the middle, round at its ends.
    const float straight = max(size.y - size.x, 0);
    const float2 off = float2(local.x, max(abs(local.z) - straight, 0));
    const float edge = length(off) - size.x;
    if (edge > 0.004) return shown;
    const float inside = 1 - smoothstep(-0.004, 0.004, edge);
    // Ridges along the deck, across its width: raised (solid) and between them thinner.
    const float phase = frac(local.x / max(ridges.x, 0.002));
    const float ridge = smoothstep(0, 0.12, phase) * (1 - smoothstep(ridges.y, ridges.y + 0.12, phase));
    const float amount = look.w * inside * lerp(0.55, 1.0, ridge);
    // Lit as the deck there is lit: its brightness, without its colour.
    const float3 lit = look.rgb * max(luminance(shown), 1e-4) * 2.5 * lerp(0.8, 1.15, ridge);
    return lerp(shown, lit, saturate(amount));
}

// How much a trail has pressed the cover away at this point: 0 none, 1 all of it.
float trail_at(float3 here) {
    const float4 head = trail[0];
    const int points = (int)head.x;
    if (points < 2) return 0;
    const float4 box = trail[1];
    if (here.x < head.y - 1 || here.z < head.z - 1 || here.x > head.w + 1 || here.z > box.x + 1 || here.y < box.y - 1 || here.y > box.z + 1)
        return 0;
    float pressed = 0;
    float4 a = trail[2];
    float strength_a = trail[3].x;
    for (int i = 1; i < points; ++i) {
        const float4 b = trail[2 + i * 2];
        const float strength_b = trail[3 + i * 2].x;
        const float2 along = b.xz - a.xz;
        const float length_squared = dot(along, along);
        // Two points far apart are not one trail (a teleport, a respawn).
        if (length_squared < 4.0 && strength_a > 0 && strength_b > 0) {
            const float t = saturate(dot(here.xz - a.xz, along) / max(length_squared, 1e-6));
            const float2 nearest = a.xz + along * t;
            const float off = length(here.xz - nearest);
            const float height = lerp(a.y, b.y, t);
            const float half_width = b.w * 0.5;
            if (abs(here.y - height) < 0.4) {
                const float strength = lerp(strength_a, strength_b, t);
                pressed = max(pressed, (1 - smoothstep(half_width * 0.6, half_width, off)) * strength);
            }
        }
        a = b;
        strength_a = strength_b;
    }
    return pressed;
}

// Snow falling: flakes on a few shells round the camera, each nearer one bigger and faster, held still
// in the world as the camera turns (they are placed by the direction of the ray), drifting down and
// with the wind, and hidden behind whatever is nearer than their shell.
float3 snowfall(float3 colour, int2 p, float scene_distance, float exposure) {
    const float amount = extra.z;
    if (amount <= 0) return colour;
    const float3 ray = normalize(ray_of(p));
    const float seconds = extra.y;
    float flakes = 0;
    [unroll] for (int k = 0; k < 5; ++k) {
        const float shell = 1.5 * pow(1.7, k);               // metres from the camera: 1.5, 2.6, 4.3, 7.4, 12.5
        if (shell > scene_distance) break;
        const float cells = 26.0 * pow(1.55, k);             // flakes across a radian: further shells, smaller flakes
        const float fall = 1.1 * max(extra.w, 0.05) / shell * cells; // about 1.1 m/s down at the usual speed, in this shell's cells
        const float3 drift = float3(0.35, 0, 0.2) / shell * cells;
        float3 q = ray * cells + float3(0, fall, 0) * seconds + drift * seconds;
        q.x += sin(seconds * 0.9 + q.y * 0.35) * 0.25;       // a little sway
        const float3 cell = floor(q);
        const float3 seed = hash3(cell + k * 17.0);
        if (seed.x > amount * 0.55) continue;                // how many cells hold a flake
        const float3 centre = cell + 0.25 + hash3(cell + 3.1) * 0.5;
        const float size = 0.11 + seed.y * 0.08;
        const float touch = 1 - smoothstep(size * 0.35, size, length(q - centre));
        flakes = max(flakes, touch * (1 - k * 0.12));
    }
    const float3 white = colour_amount.rgb * exposure * 1.6;
    return lerp(colour, white, saturate(flakes) * 0.85);
}

// The picture's brightness as a whole, for the flakes: a few points over the screen.
float exposure_now() {
    float sum = 0;
    [unroll] for (int y = 1; y <= 3; ++y)
        [unroll] for (int x = 1; x <= 3; ++x)
            sum += luminance(scene.Load(int3(int2(up_width.w * x / 4, back_height.w * y / 4), 0)).rgb);
    return max(sum / 9, 1e-4);
}

Output ps_main(Vertex input) {
    Output out_;
    const int2 p = int2(input.position.xy);
    const float4 under = scene.Load(int3(p, 0));
    out_.colour = under;
    out_.cover = float2(0, 0);
    const float d = depth_at(p);
    // The near distance the CPU holds from the measurements of many frames (eye_near.w), else this
    // frame's own from the skater's foot; 0 when neither is known.
    const float near_measured = eye_near.w > 0 ? eye_near.w : measured_near();
    const float near_distance = near_measured > 0 ? near_measured : 0.1;
    const float exposure = exposure_now();
    // The sky, and anything at the near plane (the camera inside something): snow falls in front of it.
    if (!(d > 0.0 && d < 1.0)) {
        out_.colour.rgb = snowfall(under.rgb, p, d >= 1.0 ? 0 : 1e9, exposure);
        return out_;
    }
    const float3 here = world_at(p, d, near_distance);
    const float view_distance = dot(here - eye_near.xyz, -back_height.xyz);
    const float scene_distance = length(here - eye_near.xyz);
    // The patchiness, from where the point is at a scale that never changes: with the measured one it
    // jumped whenever the measurement came and went (the skater's feet in and out of view), and the
    // snow flashed on and off with it (2026-11-10).
    const float3 steady = world_at(p, d, 0.1);
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
    float cover = 0;
    float3 shown = under.rgb; // the picture, with the deck's tread over it
    if (length_squared > 1e-20) {
        normal *= rsqrt(length_squared);
        // Facing the camera: the side the pixel is seen from.
        if (dot(normal, eye_near.xyz - here) < 0) normal = -normal;
        if (near_measured > 0) shown = tread_over(shown, here, normal);
        // Patchy where it thins: large, slow noise over the ground, in the world, so it stays put.
        const float patch = noise(steady.xz * 0.45) * 0.65 + noise(steady.xz * 1.9) * 0.35;
        const float facing = shape.x + (patch - 0.5) * shape.w * 0.6;
        const float softness = max(shape.y, 0.01);
        cover = smoothstep(facing - softness, facing + softness, normal.y) * colour_amount.w;
        // Only on surfaces flat across their neighbours, each side apart, flat if either is. A plane's
        // reversed depth changes evenly across the screen, so its second difference is nothing; leaves,
        // wires and distant edges are not flat, and the cover flickered on them. Next to an edge one
        // side is the other surface, and judged together the ground there had a dark rim (2026-11-11).
        const float dl2 = depth_at(p - int2(2, 0)), dr2 = depth_at(p + int2(2, 0));
        const float du2 = depth_at(p - int2(0, 2)), dd2 = depth_at(p + int2(0, 2));
        const float bend_across = min(abs(dl2 - 2 * dl + d), abs(d - 2 * dr + dr2));
        const float bend_along = min(abs(du2 - 2 * du + d), abs(d - 2 * dd + dd2));
        cover *= 1 - smoothstep(0.0015, 0.006, max(bend_across, bend_along) / d);
    }

    // Steadier over time: last frame's cover where this point was then (off while this_frame_share in
    // world_cover.cpp is 1: the camera it is found with can be a frame apart from the GPU's).
    if (last_right.w > 0 && last_up.w < 1) {
        const float3 to = here - last_eye.xyz;
        const float then = dot(to, -last_back.xyz);
        if (then > 0.05) {
            const float2 at = float2(up_width.w * 0.5 + dot(to, last_right.xyz) * right_focal.w / then,
                                     back_height.w * 0.5 - dot(to, last_up.xyz) * right_focal.w / then);
            if (all(at >= 0) && at.x < up_width.w && at.y < back_height.w) {
                const float2 last = last_cover.SampleLevel(linear_clamp, at / float2(up_width.w, back_height.w), 0);
                if (last.y > 0 && abs(last.y - then) < then * 0.04) cover = lerp(last.x, cover, last_up.w);
            }
        }
    }

    // Distances are only real when measured: the trails and the places kept clear need them.
    float pressed = 0;
    if (near_measured > 0 && cover > 0.002) {
        // Trails: where a board went, the snow packed down, greyer and a little thinner, filling back in
        // as the trail fades. Not taken away: the ground under it showed through blue (2026-11-11).
        pressed = trail_at(here);
        cover *= 1 - pressed * 0.2;
        // The places kept clear (the skater): upright cylinders over their feet, softened at the edge.
        // Inside one, the ground keeps its snow and whatever stands on it does not: the ground is the
        // lowest surface in a small ring round the pixel, and a point more than a centimetre or so over
        // it is something on it (the deck, drawn almost on the ground, had a white rim of snow;
        // clearing the ground too left a dark ring round the skater, 2026-11-10).
        [unroll] for (int c = 0; c < 3; ++c) {
            if (c >= (int)clear_info.x) break;
            const float3 from = here - clears[c].xyz;
            const float radius = clears[c].w;
            const float height = c == 0 ? clear_info.y : c == 1 ? clear_info.z : clear_info.w;
            const float inside = 1 - smoothstep(radius * 0.75, radius, length(from.xz));
            if (inside <= 0 || from.y < -0.5 || from.y > height) continue;
            float ground = here.y, ring = 0;
            [unroll] for (int k = 0; k < 8; ++k) {
                const float angle = k * 0.7853982;
                const int2 at = p + int2(float2(cos(angle), sin(angle)) * max(back_height.w * 0.016, 6.0));
                const float there = depth_at(at);
                if (!(there > 0)) continue;
                const float3 point_there = world_at(at, there, near_distance);
                ground = min(ground, point_there.y);
                ring = max(ring, length(point_there.xz - here.xz));
            }
            // A slope rises across the ring too: allow for one of up to about 1 in 7.
            const float over = here.y - ground - max(0.005, ring * 0.12);
            // Well above the skater's own foot it is the skater, whatever is round it on screen: from a
            // low camera the shoulders and hair have only things further up or the sky round them, and
            // kept their snow (2026-11-11).
            const float on_it = max(smoothstep(0, 0.004, over), smoothstep(0.55, 0.65, from.y));
            cover *= 1 - inside * on_it;
        }
    }
    out_.cover = float2(cover, view_distance);
    if (cover <= 0.002) {
        out_.colour.rgb = snowfall(shown, p, scene_distance, exposure);
        return out_;
    }

    // Lit as what it covers is lit, without its colour: so a shadow on the ground is a shadow on the
    // snow, and the game's exposure holds for both. Not from the pixel alone, or the ground's own
    // pattern (paving joints, cracks) shows through as grey: the middle (median) brightness of samples
    // round it on the same plane, close ones and, as a floor, wider ones. The brightest of a few samples
    // at fixed offsets copied anything bright near them onto the snow once for every offset (the deck,
    // shoes, a kerb: four to eight ghost copies round it, 2026-11-11); one thing cannot be the middle of
    // sixteen, and the samples turn a different way at every pixel, so nothing lines up into a copy.
    const float own = luminance(under.rgb);
    const float spin = hash(float2(p) * 0.618034) * 6.2831853; // the same each frame: no shimmer
    const float close = median_light(p, here, normal, near_distance, max(back_height.w * 0.008, 3.0), max(0.004, view_distance * 0.002), spin, own);
    const float wide = median_light(p, here, normal, near_distance, back_height.w * 0.03, max(0.02, view_distance * 0.01), spin + 0.2, close);
    // Not darker than about 60% of the snow round about: the picture still has the game's motion blur
    // and smear in it, faint dark copies of moving things (and the skater's soft contact shadow) that
    // showed on the snow as ghost outlines and a dark disc. Snow scatters light: its shadows are pale.
    const float light = max(lerp(close, own, 0.1), wide * 0.6);
    float3 lit = colour_amount.rgb * light * shape.z * (1 - pressed * 0.35);
    if (extra.x > 0) {
        const float glint = hash(floor(steady.xz * 38.0) + floor(steady.y * 38.0));
        lit += light * extra.x * 6.0 * step(0.996, glint);
    }
    out_.colour = float4(snowfall(lerp(shown, lit, cover), p, scene_distance, exposure), under.a);
    return out_;
}

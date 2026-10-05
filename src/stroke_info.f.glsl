// Copyright (c) 2019 Sergio Gonzalez. All rights reserved.
// License: https://github.com/serge-rgb/milton#license

in vec3 v_pointa;
in vec3 v_pointb;

uniform float u_opacity_min;
uniform float u_hardness;
uniform float u_master_alpha;
uniform int u_use_pressure;
uniform int u_use_distance;

// Opacity profile across the brush. x is distance / radius in [0, 1].
float
feather(float x, float h)
{
    float core = h * h;
    float t = clamp((x - core) / max(1.0 - core, 0.0001), 0.0, 1.0);
    float g = exp(-4.0 * t * t);
    float g1 = exp(-4.0);
    return (g - g1) / (1.0 - g1);
}

void
main()
{
    vec2 screen_point = vec2(gl_FragCoord.x, u_screen_size.y - gl_FragCoord.y);

    vec2 canvas_point = raster_to_canvas_gl(screen_point);
    vec2 a = v_pointa.xy;
    vec2 b = v_pointb.xy;

    vec2 ab = b - a;
    float len_ab = length(ab);

    float t_raw = dot((canvas_point - a)/len_ab, ab / len_ab);
    float t = clamp(t_raw, 0.0, 1.0);

    float dist;
    float rad;
    float pressure;
    float aa = float(u_scale);
    if ( u_shape == 1 ) {
        // Rectangle: dist and rad are in rectangle space (see rect_metric).
        vec2 rm = rect_metric(canvas_point, a, b);
        dist = rm.x;
        pressure = mix(v_pointa.z, v_pointb.z, rm.y);
        rad = u_radius * mix(1.0, mix(u_size_min, 1.0, pressure), float(u_size_pressure)) * u_shape_aspect;
        aa = max(length(vec2(dFdx(dist), dFdy(dist))), 0.001);
    } else {
        vec2 stroke_point = mix(a, b, t);
        pressure = mix(v_pointa.z, v_pointb.z, t);
        // Distance between fragment and stroke
        dist = distance(stroke_point, canvas_point);
        rad = u_radius * mix(1.0, mix(u_size_min, 1.0, pressure), float(u_size_pressure));
    }
    if (dist >= rad + 0.5 * aa) {
        discard;
    }
    // In the round caps, continue the pressure gradient instead of holding it flat. A flat cap
    // stamps a disc of constant opacity over the neighboring segment, which shows as banding.
    // The extrapolation is capped at the endpoint's pressure so it never exceeds it, which would
    // stamp darker discs whenever pressure is changing.
    float cap_pressure = pressure;
    if (t_raw < 0.0) {
        cap_pressure = clamp(mix(v_pointa.z, v_pointb.z, t_raw), 0.0, v_pointa.z);
    } else if (t_raw > 1.0) {
        cap_pressure = clamp(mix(v_pointa.z, v_pointb.z, t_raw), 0.0, v_pointb.z);
    }
    // Channels (see the blend setup in the renderer):
    //   R (MAX): feathered pressure opacity. It caps the accumulated opacity.
    //   G (MAX): strongest single-segment opacity at this pixel.
    //   A (ADD): arc-length weighted sum of opacities, which fills the gap between nearby passes.
    // The result is max(G, min(A, R)): a re-tread at the same pressure can never exceed R.
    float pressure_opacity = 1.0;
    if (u_use_pressure != 0) {
        pressure_opacity = (1.0 - u_opacity_min) * cap_pressure + u_opacity_min;
    }
    pressure_opacity *= u_master_alpha;
    float h = clamp((u_hardness - 1.0) / 9.0, 0.0, 1.0);
    float shape = 1.0;
    float shape_mean = 1.0;
    if (u_use_distance != 0) {
        shape = feather(clamp(dist / rad, 0.0, 1.0), h);
        shape_mean = 0.0;
        for (int k = 0; k < 8; ++k) {
            shape_mean += feather((float(k) + 0.5) / 8.0, h) / 8.0;
        }
    }
    float coverage = clamp(0.5 + (rad - dist) / aa, 0.0, 1.0);
    float alpha = coverage * pressure_opacity * shape;

    // Weight so that a straight pass sums to about the single-segment profile.
    float seg_len = len_ab;
    if ( u_shape == 1 ) {
        vec2 eq = shape_q(ab);
        seg_len = max(abs(eq.x), abs(eq.y));
    }
    float weight = min(seg_len, rad) / max(2.0 * rad * shape_mean, 0.0001);
    // The ceiling is feathered too (with a wider profile than a single segment, so it can fill the
    // gaps between passes). A flat ceiling would show as a hard-edged plateau.
    float ceiling_opacity = coverage * pressure_opacity * sqrt(shape);
    if ( u_shape == 1 ) {
        // The sum channel counts overlapping segments, which changes in steps along a rectangle's
        // slanted edge and shows as a sawtooth. Rectangles use the smooth per-segment max only.
        weight = 0.0;
    }
    out_color = vec4(ceiling_opacity, alpha, 0.0, alpha * weight);
}
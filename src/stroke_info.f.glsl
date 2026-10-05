// Copyright (c) 2019 Sergio Gonzalez. All rights reserved.
// License: https://github.com/serge-rgb/milton#license

in vec3 v_pointa;
in vec3 v_pointb;

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

    vec2 stroke_point = mix(a, b, t);

    float pressure = mix(v_pointa.z, v_pointb.z, t);

    // Distance between fragment and stroke
    float dist = distance(stroke_point, canvas_point);

    float rad = u_radius * pressure;
    if (dist >= rad + 0.5 * float(u_scale)) {
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
    out_color.r = dist / rad;
    // G holds 1 - edge coverage, so the MIN blend keeps the best coverage of overlapping segments.
    out_color.g = 1.0 - clamp(0.5 + (rad - dist) / float(u_scale), 0.0, 1.0);
    out_color.a = cap_pressure;
}

// Copyright (c) 2015 Sergio Gonzalez. All rights reserved.
// License: https://github.com/serge-rgb/milton#license

in vec3 v_pointa;
in vec3 v_pointb;

// 0: opaque core only. 1: anti-aliased rim only.
uniform int u_aa_rim;

void
main()
{
    vec2 screen_point = vec2(gl_FragCoord.x, u_screen_size.y - gl_FragCoord.y);

    vec2 canvas_point = raster_to_canvas_gl(screen_point);
    vec2 a = v_pointa.xy;
    vec2 b = v_pointb.xy;

    vec2 ab = b - a;
    float len_ab = length(ab);

    float dist;
    float aa = float(u_scale);
    if ( u_shape == 1 ) {
        vec2 rm = rect_metric(canvas_point, a, b);
        float metric = rm.x;
        float pressure = mix(v_pointa.z, v_pointb.z, rm.y);
        dist = metric - u_radius * mix(1.0, mix(u_size_min, 1.0, pressure), float(u_size_pressure)) * u_shape_aspect;
        aa = max(length(vec2(dFdx(metric), dFdy(metric))), 0.001);
    } else {
        float t = clamp(dot((canvas_point - a)/len_ab, ab / len_ab), 0.0, 1.0);
        vec2 stroke_point = mix(a, b, t);
        float pressure = mix(v_pointa.z, v_pointb.z, t);
        // Distance between fragment and stroke
        dist = distance(stroke_point, canvas_point) - u_radius * mix(1.0, mix(u_size_min, 1.0, pressure), float(u_size_pressure));
    }

    float coverage = clamp(0.5 - dist / aa, 0.0, 1.0);
    if ( u_aa_rim == 0 ) {
        if ( coverage < 1.0 ) {
            discard;
        }
        out_color = u_brush_color;
    } else {
        if ( coverage <= 0.0 || coverage >= 1.0 ) {
            discard;
        }
        out_color = u_brush_color * coverage;
    }
}
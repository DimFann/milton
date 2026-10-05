// Copyright (c) 2015 Sergio Gonzalez. All rights reserved.
// License: https://github.com/serge-rgb/milton#license


// Per-stroke uniforms
uniform vec4 u_brush_color;

// CanvasView elements:
uniform mat2 u_rotation;
uniform mat2 u_rotation_inverse;
uniform ivec2 u_pan_center;
uniform ivec2 u_zoom_center;
uniform vec2  u_screen_size;
uniform int   u_scale;
uniform int   u_radius;

// Brush shape. 0: round. 1: rectangle with its long half-extent along u_shape_axis (canvas space).
// For rectangles u_radius is the long half-extent and u_shape_aspect the short/long ratio.
uniform int   u_shape;
uniform float u_size_min;       // Minimum size fraction when pressure controls size.
uniform int   u_size_pressure;  // 0: pressure does not change the brush size.
uniform vec2  u_shape_axis;
uniform float u_shape_aspect;

vec2
canvas_to_raster_gl(vec2 cp)
{
    vec2 xy = (cp - u_pan_center) / u_scale;

    vec2 rp = ( (u_rotation_inverse * xy) + u_zoom_center ) / u_screen_size;

    // rp in [0, 1]x[0, 1]

    rp *= 2.0;
    rp -= 1.0;
    /* // rp in [-1, 1]x[-1, 1] */

    rp.y *= -1.0;

    return vec2(rp);
}

vec2
raster_to_canvas_gl(vec2 raster_point)
{
    vec2 canvas_point = (u_rotation * (raster_point - u_zoom_center) * u_scale) + vec2(u_pan_center);

    return canvas_point;
}

// Rectangle space: scaled so the rectangle becomes a square and its half-extent is
// u_radius * pressure * u_shape_aspect, with 1 unit = 1 canvas unit along the short axis.
vec2
shape_q(vec2 v)
{
    return vec2(dot(v, u_shape_axis) * u_shape_aspect,
                dot(v, vec2(-u_shape_axis.y, u_shape_axis.x)));
}

// Chebyshev distance from p to the segment ab in rectangle space. Returns (distance, closest t).
// (No out parameters: the shader prelude defines 'out' away on the GLSL 1.20 path.)
// The distance along the segment is convex and piecewise linear, so the minimum is at an endpoint
// or at one of the points where a coordinate crosses zero or the two coordinates are equal in size.
vec2
rect_metric(vec2 p, vec2 a, vec2 b)
{
    vec2 d0 = shape_q(p - a);
    vec2 e = shape_q(b - a);
    float ts[6];
    ts[0] = 0.0;
    ts[1] = 1.0;
    ts[2] = abs(e.x) > 1e-6 ? d0.x / e.x : 0.0;
    ts[3] = abs(e.y) > 1e-6 ? d0.y / e.y : 0.0;
    float s = e.x - e.y;
    ts[4] = abs(s) > 1e-6 ? (d0.x - d0.y) / s : 0.0;
    s = e.x + e.y;
    ts[5] = abs(s) > 1e-6 ? (d0.x + d0.y) / s : 0.0;
    float best = 1e30;
    float t_best = 0.0;
    for ( int i = 0; i < 6; ++i ) {
        float t = clamp(ts[i], 0.0, 1.0);
        vec2 d = d0 - t * e;
        float f = max(abs(d.x), abs(d.y));
        if ( f < best ) {
            best = f;
            t_best = t;
        }
    }
    return vec2(best, t_best);
}
// Copyright (c) 2015 Sergio Gonzalez. All rights reserved.
// License: https://github.com/serge-rgb/milton#license


uniform float u_alpha;

uniform sampler2D u_canvas;
uniform sampler2D u_opacity_history;
uniform int u_preserve_opacity;
uniform vec2 u_screen_size;

void
main()
{
    vec2 screen_point = vec2(gl_FragCoord.x, gl_FragCoord.y);
    vec2 coord = screen_point / u_screen_size;
    vec4 color = texture(u_canvas, coord);
    if ( u_preserve_opacity != 0 ) {
        float previous = texture(u_opacity_history, coord).a;
        // Add only enough coverage to reach the new maximum, never accumulate another coat.
        float extra = max(color.a - previous, 0.0) / max(1.0 - previous, 0.00001);
        color *= extra / max(color.a, 0.00001);
    }
    out_color = color;
    out_color *= u_alpha;
}

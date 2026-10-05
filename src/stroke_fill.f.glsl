// Copyright (c) 2015 Sergio Gonzalez. All rights reserved.
// License: https://github.com/serge-rgb/milton#license

uniform float u_opacity_min;
uniform float u_hardness;
uniform sampler2D u_info;

#ifndef PRESSURE_TO_OPACITY
#define PRESSURE_TO_OPACITY 1
#endif

#ifndef DISTANCE_TO_OPACITY
#define DISTANCE_TO_OPACITY 0
#endif

void
main()
{
    vec2 coord = gl_FragCoord.xy / u_screen_size;
    float alpha = texture(u_info, coord).a;
    if ( alpha > 0.0 ) {
        out_color = u_brush_color * alpha;
    }
    else {
        discard;
    }
}
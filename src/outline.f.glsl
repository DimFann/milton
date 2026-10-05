// Copyright (c) 2015 Sergio Gonzalez. All rights reserved.
// License: https://github.com/serge-rgb/milton#license

in vec2 v_sizes;

uniform int u_radius;
uniform bool u_fill;
uniform vec4 u_color;
uniform int u_shape;
uniform vec2 u_shape_axis;
uniform float u_shape_aspect;


void
main()
{
    float r = length(v_sizes);
    if ( u_shape == 1 ) {
        // Rectangle: signed distance to the box, expressed as a radius so the ring logic below is shared.
        vec2 q = vec2(dot(v_sizes, u_shape_axis), dot(v_sizes, vec2(-u_shape_axis.y, u_shape_axis.x)));
        vec2 e = abs(q) - vec2(float(u_radius), float(u_radius) * u_shape_aspect);
        r = float(u_radius) + max(e.x, e.y);
    }

    float girth = u_fill ? 2.0 : 1.5;
    const float ring_alpha = 1.0;

    if ( r <= u_radius
         && r > u_radius - girth ) {
        out_color = vec4(0,0,0,ring_alpha);
    }
    else if ( r < u_radius + girth && r >= u_radius ) {
        out_color = vec4(1,1,1,ring_alpha);
    }
    else if ( u_fill && r < u_radius ) {
        out_color = u_color;
    }
    else {
        discard;
    }
}


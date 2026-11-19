uniform sampler2D u_canvas;
uniform sampler2D u_blurred;
uniform sampler2D u_mask;
uniform vec2 u_screen_size;
uniform float u_step;
uniform int u_pass;

void main()
{
    vec2 uv = gl_FragCoord.xy / u_screen_size;
    if (u_pass == 2) {
        float strength = texture(u_mask, uv).a;
        out_color = mix(texture(u_canvas, uv), texture(u_blurred, uv), strength);
    } else {
        // Cascaded binomial filters approximate a Gaussian without skipping
        // thin paint when zoom makes the blur radius larger than a few pixels.
        vec2 axis = u_pass == 0 ? vec2(1, 0) : vec2(0, 1);
        vec2 step_uv = axis * u_step / u_screen_size;
        vec2 lo = vec2(0.5) / u_screen_size;
        vec2 hi = vec2(1.0) - lo;
        // Filter premultiplied RGBA together to avoid fringes on transparent paint.
        out_color = texture(u_canvas, uv) * (6.0 / 16.0);
        out_color += texture(u_canvas, clamp(uv - step_uv, lo, hi)) * (4.0 / 16.0);
        out_color += texture(u_canvas, clamp(uv + step_uv, lo, hi)) * (4.0 / 16.0);
        out_color += texture(u_canvas, clamp(uv - 2.0 * step_uv, lo, hi)) * (1.0 / 16.0);
        out_color += texture(u_canvas, clamp(uv + 2.0 * step_uv, lo, hi)) * (1.0 / 16.0);
    }
}

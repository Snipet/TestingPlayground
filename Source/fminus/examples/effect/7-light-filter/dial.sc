// dial.sc: the cutoff as a knob of light (in_tone its turn), brighter under the pointer and while it is held.
#include "funk/shapes.sh"

void main()
{
    vec2 c = 0.5 * u_size;
    float r = 0.42 * min(u_size.x, u_size.y);
    float lit = 0.5 + 0.25 * in_pointer_hover + 0.25 * in_pointer_down;
    float a = -135.0 + 270.0 * in_tone;
    vec3 col = mix(vec3(0.0, 0.0, 0.0), vec3(0.16, 0.16, 0.18), circle(v_px, c, r));
    col = mix(col, vec3(1.0, 0.55, 0.1) * lit, arc(v_px, c, r - 3.0, 3.0, -135.0, a));
    col = mix(col, vec3(1.0, 1.0, 1.0) * lit, needle(v_px, c, a, 0.8 * r, 2.5));
    gl_FragColor = vec4(col, circle(v_px, c, r + 1.0));
}

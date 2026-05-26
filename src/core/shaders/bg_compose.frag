// Stage C — final blend. Ported from spreed v23.0.4 WebGLCompositor.js
// (`backgroundComposite` fragment shader). Constants kept verbatim:
//   coverage = vec2(0.45, 0.7 - feather*0.01)   smoothstep edge feathering
//   lightWrapping = 0.3                          screen-blend bg edge colour
//                                                into the foreground rim
//
// Two modes selected by u_mode:
//   0 = image — blend the foreground over the supplied background image,
//       with a "light wrap" screen-blend of the background's edge colour
//       into the foreground rim so the cutout doesn't look pasted-on.
//   1 = blur  — same blend math but the "background" texture is the
//       output of bg_blur_v.frag (the camera's own blurred plate).

#version 330 core

in  vec2 v_uv;
out vec4 outColor;

uniform sampler2D u_foreground;     // raw camera RGBA
uniform sampler2D u_background;     // blurred-self (blur mode) or image (image mode)
uniform sampler2D u_personMask;     // refined mask (r-channel)
uniform vec2      u_coverage;       // smoothstep edges
uniform float     u_lightWrapping;  // 0.3 in Talk
uniform int       u_mode;           // 0 = image, 1 = blur

// Screen blend, same as Talk's: 1 - (1 - a) * (1 - b)
vec3 screen3(vec3 a, vec3 b) { return vec3(1.0) - (vec3(1.0) - a) * (vec3(1.0) - b); }

void main()
{
    vec3 fgColor = texture(u_foreground, v_uv).rgb;
    vec3 bgColor = texture(u_background, v_uv).rgb;
    float maskA  = texture(u_personMask, v_uv).r;

    // Soft alpha gradient (edge feathering, no hard threshold).
    float personMask = smoothstep(u_coverage.x, u_coverage.y, maskA);

    // Image mode: light wrap — the background's edge colour bleeds into
    // the foreground RIM only. The prior formula
    //   lightWrapping * max(0, personMask - 0.70) / 0.30
    // gave a UNIFORM 30%-of-lightWrapping tint across the entire
    // smoothstep-saturated interior (any pixel with personMask = 1
    // got the same wrap). Users reported "the bg image bleeds through
    // me" because the wrap reached the chest, not just the silhouette.
    // The bell-curve `4 * personMask * (1 - personMask)` peaks at 0.5
    // (the rim where the smoothstep is still transitioning) and falls
    // to zero at both 0 (background) and 1 (deep interior). That's
    // what "rim only" should mean.
    if (u_mode == 0) {
        float rim = personMask * (1.0 - personMask) * 4.0;   // bell, peak at 0.5
        float lightWrapMask = u_lightWrapping * rim;
        fgColor = screen3(fgColor, lightWrapMask * bgColor);
    }

    outColor = vec4(mix(bgColor, fgColor, personMask), 1.0);
}

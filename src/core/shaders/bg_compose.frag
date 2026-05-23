// Stage C — final blend. Ported from spreed v23.0.4 WebGLCompositor.js
// (`backgroundComposite` fragment shader). Constants kept verbatim:
//   coverage = [0.45, 0.7 - feather*0.01]   smoothstep edge feathering
//   lightWrapping = 0.3                     screen-blend background edge
//                                           colour into the foreground rim
//
// Phase 1 stub. Phase 2 ports the exact formula.

#version 330 core

in  vec2 v_uv;
out vec4 outColor;

uniform sampler2D u_foreground;     // camera RGBA
uniform sampler2D u_background;     // either blurred-self or chosen image
uniform sampler2D u_personMask;     // refined mask (alpha channel)
uniform vec2      u_coverage;       // smoothstep edges
uniform float     u_lightWrapping;  // 0.3 in Talk
uniform int       u_mode;           // 0 = image, 1 = blur

void main()
{
    // STUB — emit the foreground unchanged. Phase 2 implements:
    //   personMask = smoothstep(u_coverage.x, u_coverage.y, mask.a)
    //   if (mode == 0) {  // image
    //       lightWrap = max(0, personMask - cov.y) / (1 - cov.y)
    //       fg = screen(fg, lightWrap * bg)
    //   }
    //   outColor = vec4(mix(bg, fg.rgb, personMask), 1.0)
    outColor = texture(u_foreground, v_uv);
}

// Stage B — mask-aware separable Gaussian blur (horizontal pass).
// Ported from spreed v23.0.4 WebGLCompositor.js (`backgroundBlurH`).
// 9-tap kernel weights [0.227, 0.195, 0.122, 0.054, 0.016]. The "mask-aware"
// part: samples covered by the person are skipped so foreground colour
// doesn't bleed into the blurred background plate.
//
// Phase 1 stub. Phase 2 ports the exact 9-tap weighted sum.

#version 330 core

in  vec2 v_uv;
out vec4 outColor;

uniform sampler2D u_inputFrame;
uniform sampler2D u_personMask;
uniform vec2      u_texelSize;
uniform float     u_blurRadius;   // in texels

void main()
{
    // STUB — passthrough.
    outColor = texture(u_inputFrame, v_uv);
}

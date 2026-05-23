// Stage A — joint bilateral mask refinement.
// Ported from spreed v23.0.4 src/utils/media/effects/virtual-background/
//   WebGLCompositor.js (the `jointBilateralFilter` fragment shader).
// Constants kept verbatim: SIGMA_SPACE radius=5 pixels (sigmaTexel = 5/min(w,h)),
// SIGMA_COLOR=0.15. Edge-aware smoothing of the segmentation mask using
// the colour frame as a guide; removes per-frame mask flicker without an
// explicit temporal EMA.
//
// Phase 1 stub: passthrough — emits the input mask unchanged so the rest
// of the wiring can be checked in green. The full port lands in Phase 2.

#version 330 core

in  vec2 v_uv;
out vec4 outColor;

uniform sampler2D u_inputFrame;   // RGBA colour frame (guide)
uniform sampler2D u_segMask;      // r-channel mask from TFLite (0..1)
uniform vec2      u_texelSize;    // 1/width, 1/height
uniform float     u_sigmaTexel;   // = max(u_texelSize.x, u_texelSize.y) * 5.0
uniform float     u_sigmaColor;   // = 0.15

void main()
{
    // STUB — passthrough until Phase 2 ports the bilateral kernel.
    float m = texture(u_segMask, v_uv).r;
    outColor = vec4(0.0, 0.0, 0.0, m);
}

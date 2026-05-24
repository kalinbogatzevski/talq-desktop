// Stage A — joint bilateral mask refinement.
// Ported from spreed v23.0.4 src/utils/media/effects/virtual-background/
// WebGLCompositor.js (`jointBilateralFilter` fragment shader).
//
// What it does: takes the raw segmentation mask (from MediaPipe Selfie
// Segmenter, 256x256 sigmoid) and the colour camera frame, and smooths
// the mask such that its edges snap to colour discontinuities in the
// guide image. Pixels far from the center in space OR in colour get
// small weights; pixels near in both get high weights. Net effect: the
// noisy mask edges align with the actual person's silhouette instead
// of leaking into the background or eating into hair / shirt.
//
// Kernel: 7x7 (radius 3) - reduced from Talk's 4 (9x9) to cut samples
// from 81 -> 49 per pixel (~40 % less GPU work) with no perceptible
// loss in mask quality at 720p.

#version 330 core

in  vec2 v_uv;
out vec4 outColor;

uniform sampler2D u_inputFrame;   // RGBA camera frame (guide)
uniform sampler2D u_segMask;      // r-channel raw mask (0..1)
uniform vec2      u_texelSize;    // 1/width, 1/height - scales the kernel offset into UV space
uniform float     u_sigmaTexel;   // (reserved for the variable-radius variant; kernel size is hardcoded today)
uniform float     u_sigmaColor;   // normalised RGB-distance sigma, ~0.1

const int   KERNEL_RADIUS = 3;          // 7x7 sample window
const float SIGMA_SPACE_TEXELS = 2.0;   // tighter than Talk's 5 for the smaller kernel
const float TWO_SIGMA_SPACE_SQ = 2.0 * SIGMA_SPACE_TEXELS * SIGMA_SPACE_TEXELS;

void main()
{
    vec3 centerColor = texture(u_inputFrame, v_uv).rgb;
    float twoSigmaColorSq = 2.0 * u_sigmaColor * u_sigmaColor;

    float maskSum = 0.0;
    float totalW  = 0.0;

    for (int j = -KERNEL_RADIUS; j <= KERNEL_RADIUS; ++j) {
        for (int i = -KERNEL_RADIUS; i <= KERNEL_RADIUS; ++i) {
            vec2  shift = vec2(float(i), float(j)) * u_texelSize;
            vec2  coord = v_uv + shift;
            vec3  guide = texture(u_inputFrame, coord).rgb;

            // Standard bilateral: w = exp(-d^2/(2*sigma_s^2)) * exp(-dc^2/(2*sigma_c^2))
            // Combined into one exp() for the GPU. spaceDistSq is in
            // squared texel units (i,j in [-3, 3] -> 0..18).
            float spaceDistSq = float(i * i + j * j);
            vec3  colorDiff   = guide - centerColor;
            float colorDistSq = dot(colorDiff, colorDiff);

            float w = exp(
                - spaceDistSq / TWO_SIGMA_SPACE_SQ
                - colorDistSq / max(twoSigmaColorSq, 1e-5)
            );

            maskSum += texture(u_segMask, coord).r * w;
            totalW  += w;
        }
    }

    float refined = maskSum / max(totalW, 1e-5);
    // Downstream passes (blur, compose) sample the mask's .r channel,
    // so write to all three RGB components (and full alpha) so a future
    // pass that uses a different swizzle still finds the value.
    outColor = vec4(refined, refined, refined, 1.0);
}

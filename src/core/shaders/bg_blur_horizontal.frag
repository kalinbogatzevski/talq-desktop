// Stage B — mask-aware separable Gaussian blur (horizontal pass).
// Ported from spreed v23.0.4 src/utils/media/effects/virtual-background/
//   WebGLCompositor.js (the `backgroundBlur` H pass).
//
// 9-tap symmetric kernel with weights [0.227, 0.195, 0.122, 0.054, 0.016].
// "Mask-aware": each sample's contribution is scaled by (1 - personMask)
// so pixels covered by the person do NOT bleed colour into the blurred
// background plate. Where the accumulated alpha is low (most neighbours
// were person), the centre colour is used to fill in.

#version 330 core

in  vec2 v_uv;
out vec4 outColor;

uniform sampler2D u_inputFrame;
uniform sampler2D u_personMask;
uniform vec2      u_texelSize;    // 1.0 / (width, height)
uniform float     u_blurRadius;   // in texels — Talk's blurValue * width/720

const float w0 = 0.227;
const float w1 = 0.195;
const float w2 = 0.122;
const float w3 = 0.054;
const float w4 = 0.016;

vec4 takeSample(vec2 uv, float w)
{
    vec4 c    = texture(u_inputFrame, uv);
    float m   = texture(u_personMask, uv).r;
    return vec4(c.rgb, 1.0) * w * (1.0 - m);
}

void main()
{
    vec2 step = vec2(u_texelSize.x * u_blurRadius, 0.0);

    vec4 frameColor = vec4(0.0);
    frameColor += takeSample(v_uv,                 w0);
    frameColor += takeSample(v_uv + step * 1.0,    w1);
    frameColor += takeSample(v_uv - step * 1.0,    w1);
    frameColor += takeSample(v_uv + step * 2.0,    w2);
    frameColor += takeSample(v_uv - step * 2.0,    w2);
    frameColor += takeSample(v_uv + step * 3.0,    w3);
    frameColor += takeSample(v_uv - step * 3.0,    w3);
    frameColor += takeSample(v_uv + step * 4.0,    w4);
    frameColor += takeSample(v_uv - step * 4.0,    w4);

    vec3 centerColor = texture(u_inputFrame, v_uv).rgb;
    outColor = vec4(frameColor.rgb + (1.0 - frameColor.a) * centerColor, 1.0);
}

// Stage B continued — vertical pass of the mask-aware separable blur.
// Same 9-tap kernel as bg_blur_horizontal.frag, swapped axis.
//
// Phase 1 stub.

#version 330 core

in  vec2 v_uv;
out vec4 outColor;

uniform sampler2D u_inputFrame;
uniform sampler2D u_personMask;
uniform vec2      u_texelSize;
uniform float     u_blurRadius;

void main()
{
    // STUB — passthrough.
    outColor = texture(u_inputFrame, v_uv);
}

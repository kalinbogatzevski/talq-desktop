// Shared vertex shader for the background-compositor pass tree (#20).
// Talk's WebGLCompositor.js uses an equivalent passthrough — we keep the
// same vec2 attribute layout so all three fragment shaders can bind it.
//
// Phase 1 stub: identity quad. Phase 2 keeps this verbatim.

#version 330 core

layout(location = 0) in vec2 a_position;
layout(location = 1) in vec2 a_uv;

out vec2 v_uv;

void main()
{
    // Flip V so the GL texture orientation (origin bottom-left) matches
    // QImage's top-left convention. Without this, uploadTexture's
    // top-down row-major copy puts the input's last row at GL's row 0,
    // and the readback comes out vertically flipped - exactly the
    // upside-down preview the user saw when Blur / Image mode kicked
    // in, while Off mode (no compositor pass at all) stayed upright.
    v_uv = vec2(a_uv.x, 1.0 - a_uv.y);
    gl_Position = vec4(a_position, 0.0, 1.0);
}

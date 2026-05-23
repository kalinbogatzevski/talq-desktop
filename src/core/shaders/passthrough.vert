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
    v_uv = a_uv;
    gl_Position = vec4(a_position, 0.0, 1.0);
}

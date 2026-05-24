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
    // Identity UV. The Y-flip used to live HERE in 0.39.8 because the
    // single-pass pipeline produced QImage->GL->QImage with one
    // implicit GL convention flip per round-trip. 0.39.9 added an
    // intermediate FBO (the bilateral mask refine) - that re-introduced
    // a SECOND flip and put the blur plate upside-down. The clean fix
    // is to keep everything in GL-native bottom-up across all FBOs,
    // mirror QImage textures ONCE on upload (see uploadTexture in
    // BackgroundCompositor.cpp), and let fbo->toImage() flip back to
    // QImage convention on the final readback. So this shader stays
    // identity.
    v_uv = a_uv;
    gl_Position = vec4(a_position, 0.0, 1.0);
}

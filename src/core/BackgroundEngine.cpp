#include "BackgroundEngine.h"

// Phase 1 stub. The real engine — TFLite interpreter + selfie_segmenter.tflite
// + Qt OpenGL FBO + three GLSL fragment shaders ported from Talk web's
// WebGLCompositor.js — lands in Phase 2 / Phase 3 per
// docs/superpowers/plans/2026-05-24-video-background.md. For now this
// pass-through lets PublishPipeline wire the call site without dragging
// in a half-finished engine.

BackgroundEngine::BackgroundEngine(QObject *parent)
    : QObject(parent)
{}

BackgroundEngine::~BackgroundEngine() = default;

void BackgroundEngine::setMode(Mode m)        { m_mode = m; }
BackgroundEngine::Mode BackgroundEngine::mode() const { return m_mode; }

void BackgroundEngine::setBlurStrength(int s) { m_blurStrength = qBound(1, s, 20); }
int  BackgroundEngine::blurStrength() const   { return m_blurStrength; }

void BackgroundEngine::setImagePath(const QString &p) { m_imagePath = p; }
QString BackgroundEngine::imagePath() const           { return m_imagePath; }

QImage BackgroundEngine::processFrame(const QImage &rgba)
{
    // Stub: pass-through. The composite pipeline is wired in Phase 2.
    return rgba;
}

bool BackgroundEngine::isReady() const
{
    // No backend yet. Phase 2 flips this to reflect interpreter readiness.
    return false;
}

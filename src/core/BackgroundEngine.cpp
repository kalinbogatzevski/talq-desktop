#include "BackgroundEngine.h"
#include "BackgroundCompositor.h"

// Phase 2a additions: owns a BackgroundCompositor (lazy-constructed on
// first non-None mode so a None-mode publisher pays zero startup cost).
// The compositor is the QOpenGL FBO/shader chain ported from Talk's
// WebGLCompositor.js; today it's still a skeleton — see compositor
// header. processFrame() remains pass-through until Phase 2c.

BackgroundEngine::BackgroundEngine(QObject *parent)
    : QObject(parent)
{}

BackgroundEngine::~BackgroundEngine()
{
    delete m_compositor;
}

void BackgroundEngine::setMode(Mode m)
{
    m_mode = m;
    // Lazy-construct the compositor when the user actually opts in. Pure
    // Off-mode publishers (the vast majority today) pay nothing for this.
    if (m != Mode::None && !m_compositor)
        m_compositor = new BackgroundCompositor(this);
}
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
    // Phase 2a: ready when the compositor exists and reports init success.
    // Phase 2d adds the TFLite interpreter readiness check.
    return m_compositor && m_compositor->isReady();
}

#include "BackgroundCompositor.h"

#include <QOpenGLContext>
#include <QOffscreenSurface>
#include <QOpenGLFramebufferObject>
#include <QSurfaceFormat>
#include <QDebug>

// Phase 2a — API skeleton. Phase 2b lands ensureInitialised(), shader
// compilation, and FBO allocation; Phase 2c lands the three-pass render
// inside the composite*() methods. Until then the methods report the
// not-yet-implemented path so the engine falls back to passthrough.

BackgroundCompositor::BackgroundCompositor(QObject *parent)
    : QObject(parent)
{}

BackgroundCompositor::~BackgroundCompositor()
{
    // Phase 2b will tear down FBOs + GL context here. Forward-declared
    // pointers are nullptr today; nothing to release.
}

bool BackgroundCompositor::ensureInitialised()
{
    if (m_ready) return true;
    // Phase 2b implements:
    //   1. Choose QSurfaceFormat (GL 3.3 core, RGBA8 + depth16).
    //   2. Construct QOffscreenSurface + create()
    //   3. Construct QOpenGLContext + setFormat + create()
    //   4. makeCurrent() on the surface
    //   5. Compile + link the four shader programs from :/bg/shaders/
    //   6. Allocate the five FBOs at a sentinel resolution (resized on
    //      first frame in ensureFbos(size)).
    //   7. On any step's failure, emit initFailed() and return false.
    qDebug() << "BackgroundCompositor::ensureInitialised() — Phase 2b stub";
    return false;
}

QImage BackgroundCompositor::compositeBlur(const QImage &rgba,
                                            const QImage & /*mask*/,
                                            float /*radius*/)
{
    // Phase 2c will:
    //   - upload rgba to m_fboInput
    //   - upload mask to a texture, run bg_mask_refine through m_fboMask
    //   - run bg_blur_horizontal then _vertical (m_fboBlurH/V)
    //   - run bg_compose into m_fboOutput
    //   - read m_fboOutput back to a QImage
    if (!ensureInitialised()) return rgba;
    return rgba;
}

QImage BackgroundCompositor::compositeImage(const QImage &rgba,
                                             const QImage & /*mask*/,
                                             const QImage & /*bg*/)
{
    if (!ensureInitialised()) return rgba;
    return rgba;
}

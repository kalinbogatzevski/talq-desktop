#include "BackgroundCompositor.h"

#include <QOpenGLContext>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <QColorSpace>
#include <QFile>
#include <QDebug>
#include <QResource>

namespace {
// Load a GLSL source file from the :/bg/shaders/ qrc prefix (registered
// via resources/backgrounds.qrc in Phase 1) as a QByteArray. Returns
// empty on miss; caller treats that as a fatal init error.
QByteArray loadShaderSource(const char *aliasName)
{
    QFile f(QStringLiteral(":/bg/shaders/") + QString::fromLatin1(aliasName));
    if (!f.open(QIODevice::ReadOnly)) {
        qWarning() << "BackgroundCompositor: shader resource missing:"
                   << aliasName;
        return {};
    }
    return f.readAll();
}

// Compile + link a (vertex, fragment) program. The vertex shader is the
// same passthrough.vert for every program. Returns nullptr + logs on
// any failure so ensureInitialised can fail fast and the engine falls
// back to pass-through.
QOpenGLShaderProgram *buildProgram(const QByteArray &vertSrc,
                                    const QByteArray &fragSrc,
                                    const char *name)
{
    auto *prog = new QOpenGLShaderProgram;
    if (!prog->addShaderFromSourceCode(QOpenGLShader::Vertex, vertSrc)) {
        qWarning() << "BackgroundCompositor:" << name
                   << "vertex compile failed:" << prog->log();
        delete prog; return nullptr;
    }
    if (!prog->addShaderFromSourceCode(QOpenGLShader::Fragment, fragSrc)) {
        qWarning() << "BackgroundCompositor:" << name
                   << "fragment compile failed:" << prog->log();
        delete prog; return nullptr;
    }
    if (!prog->link()) {
        qWarning() << "BackgroundCompositor:" << name
                   << "link failed:" << prog->log();
        delete prog; return nullptr;
    }
    qInfo() << "BackgroundCompositor:" << name << "compiled + linked";
    return prog;
}
} // namespace

BackgroundCompositor::BackgroundCompositor(QObject *parent)
    : QObject(parent)
{}

BackgroundCompositor::~BackgroundCompositor()
{
    releaseAll();
}

void BackgroundCompositor::releaseAll()
{
    // If we ever held a GL context, make it current so program/FBO
    // destruction releases GL handles correctly. If we never did, the
    // pointers are nullptr and these deletes are no-ops.
    if (m_glCtx && m_surface)
        m_glCtx->makeCurrent(m_surface);

    delete m_progMaskRefine; m_progMaskRefine = nullptr;
    delete m_progBlurH;      m_progBlurH      = nullptr;
    delete m_progBlurV;      m_progBlurV      = nullptr;
    delete m_progCompose;    m_progCompose    = nullptr;

    delete m_fboInput;  m_fboInput  = nullptr;
    delete m_fboMask;   m_fboMask   = nullptr;
    delete m_fboBlurH;  m_fboBlurH  = nullptr;
    delete m_fboBlurV;  m_fboBlurV  = nullptr;
    delete m_fboOutput; m_fboOutput = nullptr;

    if (m_glCtx) {
        m_glCtx->doneCurrent();
        delete m_glCtx;
        m_glCtx = nullptr;
    }
    delete m_surface; m_surface = nullptr;
    m_ready = false;
}

bool BackgroundCompositor::createContext()
{
    // GL 3.3 core — the lowest profile that supports both the GLSL
    // `#version 330 core` shaders we ship and `layout(location=…)` in
    // the vertex shader. Universally available on Windows desktop GPUs.
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setMajorVersion(3);
    fmt.setMinorVersion(3);
    fmt.setColorSpace(QColorSpace::SRgb);
    fmt.setDepthBufferSize(0);   // compositor is 2D — no depth needed
    fmt.setStencilBufferSize(0);
    fmt.setRedBufferSize(8);
    fmt.setGreenBufferSize(8);
    fmt.setBlueBufferSize(8);
    fmt.setAlphaBufferSize(8);

    m_surface = new QOffscreenSurface;
    m_surface->setFormat(fmt);
    m_surface->create();
    if (!m_surface->isValid()) {
        qWarning() << "BackgroundCompositor: QOffscreenSurface invalid "
                      "(no GL3.3 platform support?)";
        emit initFailed(QStringLiteral(
            "Background processing needs OpenGL 3.3 — your GPU/driver "
            "couldn't open an offscreen surface."));
        return false;
    }

    m_glCtx = new QOpenGLContext;
    m_glCtx->setFormat(fmt);
    if (!m_glCtx->create()) {
        qWarning() << "BackgroundCompositor: QOpenGLContext::create() failed";
        emit initFailed(QStringLiteral(
            "Background processing needs OpenGL 3.3 — context creation failed."));
        return false;
    }
    if (!m_glCtx->makeCurrent(m_surface)) {
        qWarning() << "BackgroundCompositor: makeCurrent failed";
        emit initFailed(QStringLiteral(
            "Background processing needs OpenGL 3.3 — couldn't activate context."));
        return false;
    }
    qInfo().nospace() << "BackgroundCompositor: GL "
                      << reinterpret_cast<const char *>(
                             m_glCtx->functions()->glGetString(GL_VERSION))
                      << " context ready (renderer: "
                      << reinterpret_cast<const char *>(
                             m_glCtx->functions()->glGetString(GL_RENDERER))
                      << ")";
    return true;
}

bool BackgroundCompositor::compilePrograms()
{
    const QByteArray vert = loadShaderSource("passthrough.vert");
    if (vert.isEmpty()) return false;

    m_progMaskRefine = buildProgram(vert, loadShaderSource("bg_mask_refine.frag"),    "bg_mask_refine");
    m_progBlurH      = buildProgram(vert, loadShaderSource("bg_blur_horizontal.frag"), "bg_blur_horizontal");
    m_progBlurV      = buildProgram(vert, loadShaderSource("bg_blur_vertical.frag"),   "bg_blur_vertical");
    m_progCompose    = buildProgram(vert, loadShaderSource("bg_compose.frag"),         "bg_compose");

    return m_progMaskRefine && m_progBlurH && m_progBlurV && m_progCompose;
}

bool BackgroundCompositor::ensureInitialised()
{
    if (m_ready) return true;

    if (!createContext()) {
        releaseAll();
        return false;
    }
    if (!compilePrograms()) {
        emit initFailed(QStringLiteral(
            "Background processing — one of the shader programs failed to "
            "compile or link. See the log for the GLSL error."));
        releaseAll();
        return false;
    }

    // FBO allocation is deferred to the first compositeBlur/Image call
    // when we actually know the camera resolution. m_lastSize stays
    // empty until then.
    m_ready = true;
    qInfo() << "BackgroundCompositor: initialised";
    return true;
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

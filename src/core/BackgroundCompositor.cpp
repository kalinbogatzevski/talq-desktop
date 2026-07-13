#include "BackgroundCompositor.h"

#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLFunctions>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLShaderProgram>
#include <QOpenGLTexture>
#include <QOpenGLVertexArrayObject>
#include <QOffscreenSurface>
#include <QSurfaceFormat>
#include <QColorSpace>
#include <QFile>
#include <QDebug>
#include <QImage>
#include <QVector2D>

namespace {

// Load a GLSL source file from the :/bg/shaders/ qrc prefix (registered
// via resources/backgrounds.qrc in Phase 1) as a QByteArray.
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
    // Bind the attribute locations BEFORE link to match passthrough.vert's
    // explicit layout(location=…) declarations on platforms that ignore them.
    prog->bindAttributeLocation("a_position", 0);
    prog->bindAttributeLocation("a_uv",       1);
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

void BackgroundCompositor::setExternalSurface(QOffscreenSurface *surface)
{
    m_surface = surface;
    m_surfaceExternal = (surface != nullptr);
}

void BackgroundCompositor::releaseAll()
{
    if (m_glCtx && m_surface)
        m_glCtx->makeCurrent(m_surface);

    delete m_texFg;   m_texFg   = nullptr;
    delete m_texMask; m_texMask = nullptr;
    delete m_texBg;   m_texBg   = nullptr;
    m_texBgCacheKey = 0;

    delete m_vbo; m_vbo = nullptr;
    if (m_vao) { m_vao->destroy(); delete m_vao; m_vao = nullptr; }

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
    // 0.40.1 — only delete the surface if we created it ourselves. With
    // the worker-thread refactor the engine owns the surface (created on
    // Qt main, then moved to the worker thread) and outlives the
    // compositor across mode toggles.
    //
    // 0.60.2 (2026-07-13 field RCA) — and only NULL it in that case too.
    // The old code nulled m_surface unconditionally, so after a failed GL
    // init with an external surface the next composite's createContext()
    // saw m_surface == nullptr and new'd a FRESH QOffscreenSurface on the
    // worker thread (QOffscreenSurface must not be created/destroyed off
    // the GUI thread on Windows — see BackgroundEngine's ctor comment)…
    // and then leaked it on the following releaseAll(), because
    // m_surfaceExternal was still true so the delete was skipped. Net:
    // one leaked surface + one context rebuild PER FRAME while init kept
    // failing. Keeping the pointer means a retry (now latched anyway, see
    // ensureInitialised) reuses the engine's GUI-thread-created surface.
    if (!m_surfaceExternal) {
        delete m_surface;
        m_surface = nullptr;
    }
    m_ready = false;
    m_lastSize = QSize();
}

bool BackgroundCompositor::createContext()
{
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setMajorVersion(3);
    fmt.setMinorVersion(3);
    fmt.setColorSpace(QColorSpace::SRgb);
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(0);
    fmt.setRedBufferSize(8);
    fmt.setGreenBufferSize(8);
    fmt.setBlueBufferSize(8);
    fmt.setAlphaBufferSize(8);

    // 0.40.1 — when an external surface was supplied (engine creates it
    // on Qt main, then moves it to the worker thread), reuse it as-is.
    // The legacy path stays for callers that construct the compositor
    // without a worker (unit tests, headless harness).
    if (!m_surface) {
        m_surface = new QOffscreenSurface;
        m_surface->setFormat(fmt);
        m_surface->create();
    }
    if (!m_surface->isValid()) {
        emit initFailed(QStringLiteral(
            "Background processing needs OpenGL 3.3 — couldn't open an offscreen surface."));
        return false;
    }
    m_glCtx = new QOpenGLContext;
    m_glCtx->setFormat(fmt);
    if (!m_glCtx->create() || !m_glCtx->makeCurrent(m_surface)) {
        emit initFailed(QStringLiteral(
            "Background processing needs OpenGL 3.3 — context creation failed."));
        return false;
    }
    qInfo().nospace()
        << "BackgroundCompositor: GL "
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

bool BackgroundCompositor::createGeometry()
{
    static const float quadVerts[] = {
        -1.f, -1.f,  0.f, 0.f,
         1.f, -1.f,  1.f, 0.f,
        -1.f,  1.f,  0.f, 1.f,
         1.f,  1.f,  1.f, 1.f,
    };

    m_vao = new QOpenGLVertexArrayObject;
    if (!m_vao->create()) {
        qWarning() << "BackgroundCompositor: VAO create failed";
        return false;
    }
    m_vao->bind();

    m_vbo = new QOpenGLBuffer(QOpenGLBuffer::VertexBuffer);
    if (!m_vbo->create()) {
        qWarning() << "BackgroundCompositor: VBO create failed";
        m_vao->release();
        return false;
    }
    m_vbo->setUsagePattern(QOpenGLBuffer::StaticDraw);
    m_vbo->bind();
    m_vbo->allocate(quadVerts, sizeof(quadVerts));

    auto *f = m_glCtx->extraFunctions();
    f->glEnableVertexAttribArray(0);
    f->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE,
                             4 * sizeof(float),
                             reinterpret_cast<void *>(0));
    f->glEnableVertexAttribArray(1);
    f->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE,
                             4 * sizeof(float),
                             reinterpret_cast<void *>(2 * sizeof(float)));

    m_vbo->release();
    m_vao->release();
    return true;
}

bool BackgroundCompositor::ensureFbos(const QSize &size)
{
    if (size == m_lastSize && m_fboInput) return true;

    // Tear down old FBOs if the size changed.
    delete m_fboInput;  m_fboInput  = nullptr;
    delete m_fboMask;   m_fboMask   = nullptr;
    delete m_fboBlurH;  m_fboBlurH  = nullptr;
    delete m_fboBlurV;  m_fboBlurV  = nullptr;
    delete m_fboOutput; m_fboOutput = nullptr;

    QOpenGLFramebufferObjectFormat fmt;
    fmt.setInternalTextureFormat(GL_RGBA8);
    fmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    fmt.setSamples(0);

    m_fboInput  = new QOpenGLFramebufferObject(size, fmt);
    m_fboMask   = new QOpenGLFramebufferObject(size, fmt);
    m_fboBlurH  = new QOpenGLFramebufferObject(size, fmt);
    m_fboBlurV  = new QOpenGLFramebufferObject(size, fmt);
    m_fboOutput = new QOpenGLFramebufferObject(size, fmt);

    if (!m_fboInput->isValid() || !m_fboMask->isValid()
        || !m_fboBlurH->isValid() || !m_fboBlurV->isValid()
        || !m_fboOutput->isValid()) {
        qWarning() << "BackgroundCompositor: FBO allocation failed for"
                   << size;
        return false;
    }
    m_lastSize = size;
    return true;
}

bool BackgroundCompositor::ensureInitialised()
{
    if (m_ready) return true;
    // 0.60.2 (2026-07-13 field RCA) — fail ONCE, then stay failed. Without
    // this latch every composite call (15-30 per second) re-attempted the
    // whole context/shader init, re-emitted initFailed each time, and (with
    // the external-surface bug in releaseAll, fixed alongside) leaked a
    // QOffscreenSurface per frame. GL 3.3 capability doesn't appear
    // mid-session; the user toggling the background Off→On constructs a
    // fresh compositor, which is the natural retry point.
    if (m_initFailedPermanently) return false;
    if (!createContext()) {          // createContext emits initFailed itself
        m_initFailedPermanently = true;
        releaseAll();
        return false;
    }
    if (!compilePrograms()) {
        emit initFailed(QStringLiteral(
            "Background processing — a shader program failed to compile or link."));
        m_initFailedPermanently = true;
        releaseAll();
        return false;
    }
    if (!createGeometry()) {
        emit initFailed(QStringLiteral(
            "Background processing — couldn't allocate the full-screen quad."));
        m_initFailedPermanently = true;
        releaseAll();
        return false;
    }
    m_ready = true;
    qInfo() << "BackgroundCompositor: initialised";
    return true;
}

void BackgroundCompositor::uploadTexture(QOpenGLTexture *&tex,
                                          const QImage &img,
                                          bool singleChannel)
{
    // Re-use existing texture when shape + format match — at 30 fps with
    // two textures per frame, create/destroy was a steady stall (review I3).
    // Only re-allocate when the input dimensions change.
    const auto wantFormat = singleChannel ? QOpenGLTexture::R8_UNorm
                                           : QOpenGLTexture::RGBA8_UNorm;
    if (!tex
        || tex->width()  != img.width()
        || tex->height() != img.height()
        || tex->format() != wantFormat) {
        delete tex;
        tex = new QOpenGLTexture(QOpenGLTexture::Target2D);
        tex->setFormat(wantFormat);
        tex->setSize(img.width(), img.height());
        tex->setMipLevels(1);
        tex->setMinificationFilter(QOpenGLTexture::Linear);
        tex->setMagnificationFilter(QOpenGLTexture::Linear);
        tex->setWrapMode(QOpenGLTexture::ClampToEdge);
        tex->allocateStorage();
    }

    // Vertical mirror on upload so the GL texture is stored bottom-up
    // (GL convention: UV (0,0) at bottom-left). With identity UV in
    // passthrough.vert, intermediate FBOs all stay in GL-native
    // orientation, and the final readbackFbo() / fbo->toImage() does
    // one Y-flip back to QImage's top-left convention. Was previously
    // handled by a `v_uv.y = 1.0 - a_uv.y` in the vertex shader, but
    // that broke once the pipeline grew an intermediate FBO (bilateral
    // refine) because every FBO read would re-flip.
    if (singleChannel) {
        QImage gray = img.format() == QImage::Format_Grayscale8
            ? img : img.convertToFormat(QImage::Format_Grayscale8);
        gray = gray.mirrored(false, true);
        tex->setData(QOpenGLTexture::Red, QOpenGLTexture::UInt8,
                     gray.constBits(),
                     /*pixel-transfer-options*/ nullptr);
    } else {
        QImage rgba = img.format() == QImage::Format_RGBA8888
            ? img : img.convertToFormat(QImage::Format_RGBA8888);
        rgba = rgba.mirrored(false, true);
        tex->setData(QOpenGLTexture::RGBA, QOpenGLTexture::UInt8,
                     rgba.constBits(),
                     nullptr);
    }
}

void BackgroundCompositor::runPass(QOpenGLShaderProgram *prog,
                                    QOpenGLFramebufferObject *target)
{
    // Caller has already prog->bind()'d and set uniforms with the program
    // active — we just bind the FBO, set viewport, clear, draw, release.
    auto *f = m_glCtx->functions();
    target->bind();
    f->glViewport(0, 0, target->width(), target->height());
    f->glClearColor(0.f, 0.f, 0.f, 1.f);
    f->glClear(GL_COLOR_BUFFER_BIT);
    m_vao->bind();
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_vao->release();
    prog->release();
    target->release();
}

QImage BackgroundCompositor::readbackFbo(QOpenGLFramebufferObject *fbo)
{
    // QOpenGLFramebufferObject::toImage flips Y for us. Returns ARGB32_Premultiplied;
    // the caller converts back to RGBA8888 if it wants to compare per-channel.
    QImage img = fbo->toImage();
    return img.convertToFormat(QImage::Format_RGBA8888);
}

// Talk's smoothstep cutoff for the mask edge. The "feather" / "person
// size" term scales the inner cutoff; Talk's reference uses 0 by default
// (no feather adjustment). Named so the next reader doesn't read the
// hardcoded `0.70 - 0 * 0.01` as a half-finished port (review I6).
namespace {
// Talk's WebGLCompositor.js defaults for the smoothstep edge transition.
// These assume a bilateral-refined mask (their jointBilateralFilter runs
// before compose). Our 0.39.5 release widened to (0.35, 0.75) as a
// noise-hiding workaround while bg_mask_refine.frag was still a stub;
// 0.39.9 lands the real bilateral pass, so we narrow back to Talk's
// values for crisper silhouette edges.
constexpr float kCoverageLow    = 0.45f;
constexpr float kCoverageHigh   = 0.70f;
constexpr float kEdgeFeatherPx  = 0.0f;
// Back to Talk's default. The reduction to 0.10 in 0.40.0-RC was
// treating the symptom; the actual fix is in bg_compose.frag where
// lightWrapMask now uses a rim-only bell curve instead of the old
// "uniform over the smoothstep-saturated interior" formula. With the
// rim gate in place, 0.30 stays well-contained at the silhouette
// boundary and matches Talk's intended halo strength.
constexpr float kLightWrapping  = 0.30f;
} // namespace

QImage BackgroundCompositor::compositeBlur(const QImage &rgba,
                                            const QImage &mask,
                                            float radius)
{
    if (!ensureInitialised()) return rgba;
    if (rgba.isNull() || mask.isNull()) {
        qWarning() << "BackgroundCompositor::compositeBlur — null input";
        return rgba;
    }
    if (!m_glCtx->makeCurrent(m_surface)) {
        qWarning() << "BackgroundCompositor::compositeBlur — makeCurrent failed";
        return rgba;
    }
    if (!ensureFbos(rgba.size())) {
        qWarning() << "BackgroundCompositor::compositeBlur — FBO alloc failed for"
                   << rgba.size();
        return rgba;
    }

    uploadTexture(m_texFg,   rgba, /*singleChannel*/ false);
    uploadTexture(m_texMask, mask, /*singleChannel*/ true);

    auto *f = m_glCtx->extraFunctions();
    const QSize sz = rgba.size();
    const QVector2D texelSize(1.0f / sz.width(), 1.0f / sz.height());

    // Pass 0: joint bilateral mask refine. Snaps the raw segmentation
    // mask edges to colour discontinuities in the camera frame, which
    // gives a much cleaner silhouette than the upsampled sigmoid alone.
    // Output goes into m_fboMask; subsequent passes sample that FBO's
    // colour texture instead of m_texMask.
    f->glActiveTexture(GL_TEXTURE0);
    m_texFg->bind();
    f->glActiveTexture(GL_TEXTURE1);
    m_texMask->bind();
    m_progMaskRefine->bind();
    m_progMaskRefine->setUniformValue("u_inputFrame", 0);
    m_progMaskRefine->setUniformValue("u_segMask",    1);
    m_progMaskRefine->setUniformValue("u_texelSize",  texelSize);
    m_progMaskRefine->setUniformValue("u_sigmaTexel",
        qMax(texelSize.x(), texelSize.y()) * 5.0f);
    m_progMaskRefine->setUniformValue("u_sigmaColor", 0.1f);
    runPass(m_progMaskRefine, m_fboMask);

    // Pass 1: horizontal blur. Uniforms are set INSIDE the bound/release
    // bracket so glUniform* always reaches an active program (review B1).
    // Reads the REFINED mask from m_fboMask, not m_texMask.
    f->glActiveTexture(GL_TEXTURE0);
    m_texFg->bind();
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, m_fboMask->texture());
    m_progBlurH->bind();
    m_progBlurH->setUniformValue("u_inputFrame", 0);
    m_progBlurH->setUniformValue("u_personMask", 1);
    m_progBlurH->setUniformValue("u_texelSize",  texelSize);
    m_progBlurH->setUniformValue("u_blurRadius", radius);
    runPass(m_progBlurH, m_fboBlurH);

    // Pass 2: vertical blur of pass-1 result. Mask is the refined one.
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, m_fboBlurH->texture());
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, m_fboMask->texture());
    m_progBlurV->bind();
    m_progBlurV->setUniformValue("u_inputFrame", 0);
    m_progBlurV->setUniformValue("u_personMask", 1);
    m_progBlurV->setUniformValue("u_texelSize",  texelSize);
    m_progBlurV->setUniformValue("u_blurRadius", radius);
    runPass(m_progBlurV, m_fboBlurV);

    // Pass 3: compose sharp foreground over blurred background by mask.
    // The mask sampled here is the BILATERAL-REFINED one from pass 0,
    // not the raw segmentation. This is what gives the crisp silhouette.
    f->glActiveTexture(GL_TEXTURE0);
    m_texFg->bind();
    f->glActiveTexture(GL_TEXTURE1);
    f->glBindTexture(GL_TEXTURE_2D, m_fboBlurV->texture());
    f->glActiveTexture(GL_TEXTURE2);
    f->glBindTexture(GL_TEXTURE_2D, m_fboMask->texture());
    m_progCompose->bind();
    m_progCompose->setUniformValue("u_foreground",   0);
    m_progCompose->setUniformValue("u_background",   1);
    m_progCompose->setUniformValue("u_personMask",   2);
    m_progCompose->setUniformValue("u_coverage",
        QVector2D(kCoverageLow, kCoverageHigh - kEdgeFeatherPx * 0.01f));
    m_progCompose->setUniformValue("u_lightWrapping", kLightWrapping);
    m_progCompose->setUniformValue("u_mode", 1);   // blur mode
    runPass(m_progCompose, m_fboOutput);

    return readbackFbo(m_fboOutput);
}

QImage BackgroundCompositor::compositeImage(const QImage &rgba,
                                             const QImage &mask,
                                             const QImage &bg)
{
    if (!ensureInitialised()) return rgba;
    if (rgba.isNull() || mask.isNull() || bg.isNull()) {
        qWarning() << "BackgroundCompositor::compositeImage — null input";
        return rgba;
    }
    if (!m_glCtx->makeCurrent(m_surface)) {
        qWarning() << "BackgroundCompositor::compositeImage — makeCurrent failed";
        return rgba;
    }
    if (!ensureFbos(rgba.size())) {
        qWarning() << "BackgroundCompositor::compositeImage — FBO alloc failed for"
                   << rgba.size();
        return rgba;
    }

    uploadTexture(m_texFg,   rgba, false);
    uploadTexture(m_texMask, mask, true);

    // Re-upload the background texture whenever the IMAGE CONTENT changes
    // — keying on size alone let two same-dimension backgrounds silently
    // share the stale GPU texture (review B2). QImage::cacheKey is bumped
    // on any pixel-data change (incl. assignment).
    if (!m_texBg || m_texBgCacheKey != bg.cacheKey()) {
        uploadTexture(m_texBg, bg, false);
        m_texBgCacheKey = bg.cacheKey();
    }

    auto *f = m_glCtx->extraFunctions();
    const QSize sz = rgba.size();
    const QVector2D texelSize(1.0f / sz.width(), 1.0f / sz.height());

    // Pass 0: joint bilateral mask refine (same kernel as blur mode).
    // The refined mask lands in m_fboMask and is sampled by the compose
    // pass below.
    f->glActiveTexture(GL_TEXTURE0);
    m_texFg->bind();
    f->glActiveTexture(GL_TEXTURE1);
    m_texMask->bind();
    m_progMaskRefine->bind();
    m_progMaskRefine->setUniformValue("u_inputFrame", 0);
    m_progMaskRefine->setUniformValue("u_segMask",    1);
    m_progMaskRefine->setUniformValue("u_texelSize",  texelSize);
    m_progMaskRefine->setUniformValue("u_sigmaTexel",
        qMax(texelSize.x(), texelSize.y()) * 5.0f);
    m_progMaskRefine->setUniformValue("u_sigmaColor", 0.1f);
    runPass(m_progMaskRefine, m_fboMask);

    // Pass 1: compose foreground over background image, using the
    // refined mask for the smoothstep + lightWrapping blend.
    f->glActiveTexture(GL_TEXTURE0);
    m_texFg->bind();
    f->glActiveTexture(GL_TEXTURE1);
    m_texBg->bind();
    f->glActiveTexture(GL_TEXTURE2);
    f->glBindTexture(GL_TEXTURE_2D, m_fboMask->texture());

    m_progCompose->bind();
    m_progCompose->setUniformValue("u_foreground",   0);
    m_progCompose->setUniformValue("u_background",   1);
    m_progCompose->setUniformValue("u_personMask",   2);
    m_progCompose->setUniformValue("u_coverage",
        QVector2D(kCoverageLow, kCoverageHigh - kEdgeFeatherPx * 0.01f));
    m_progCompose->setUniformValue("u_lightWrapping", kLightWrapping);
    m_progCompose->setUniformValue("u_mode", 0);   // image mode
    runPass(m_progCompose, m_fboOutput);

    return readbackFbo(m_fboOutput);
}

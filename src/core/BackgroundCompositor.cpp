#include "BackgroundCompositor.h"

#include <QOpenGLBuffer>
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QOpenGLFramebufferObject>
#include <QOpenGLFramebufferObjectFormat>
#include <QOpenGLFunctions>
#include <QOpenGLFunctions_3_3_Core>
#include <QOpenGLPixelTransferOptions>
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

#include <cstring>

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

std::atomic<int> BackgroundCompositor::s_liveInstances{0};

int BackgroundCompositor::liveInstanceCount()
{
    return s_liveInstances.load(std::memory_order_acquire);
}

BackgroundCompositor::BackgroundCompositor(QObject *parent)
    : QObject(parent)
{
    s_liveInstances.fetch_add(1, std::memory_order_acq_rel);
}

BackgroundCompositor::~BackgroundCompositor()
{
    releaseAll();
    // Decrement AFTER releaseAll so an observer that reads 0 knows the GL
    // resources are freed, not merely doomed.
    s_liveInstances.fetch_sub(1, std::memory_order_acq_rel);
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

    delete m_fboMask;   m_fboMask   = nullptr;
    delete m_fboBlurH;  m_fboBlurH  = nullptr;
    delete m_fboBlurV;  m_fboBlurV  = nullptr;
    delete m_fboOutput; m_fboOutput = nullptr;

    if (m_glCtx && m_pbo[0]) {
        m_glCtx->extraFunctions()->glDeleteBuffers(2, m_pbo);
        m_pbo[0] = m_pbo[1] = 0;
    }
    m_pboFilled[0] = m_pboFilled[1] = false;
    m_pboNext = 0;

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
    if (size == m_lastSize && m_fboOutput) return true;

    // Tear down old FBOs if the size changed.
    delete m_fboMask;   m_fboMask   = nullptr;
    delete m_fboBlurH;  m_fboBlurH  = nullptr;
    delete m_fboBlurV;  m_fboBlurV  = nullptr;
    delete m_fboOutput; m_fboOutput = nullptr;

    QOpenGLFramebufferObjectFormat fmt;
    fmt.setInternalTextureFormat(GL_RGBA8);
    fmt.setAttachment(QOpenGLFramebufferObject::NoAttachment);
    fmt.setSamples(0);

    m_fboMask   = new QOpenGLFramebufferObject(size, fmt);
    m_fboBlurH  = new QOpenGLFramebufferObject(size, fmt);
    m_fboBlurV  = new QOpenGLFramebufferObject(size, fmt);
    m_fboOutput = new QOpenGLFramebufferObject(size, fmt);

    if (!m_fboMask->isValid()
        || !m_fboBlurH->isValid() || !m_fboBlurV->isValid()
        || !m_fboOutput->isValid()) {
        qWarning() << "BackgroundCompositor: FBO allocation failed for"
                   << size;
        return false;
    }

    // Size the readback PBO pair to match. A pending readback (if any) is
    // for the OLD size — its flags are cleared with the reallocation, so
    // the next composite takes the synchronous path once. A resolution
    // change mid-call is also a producer boundary, so dropping the parked
    // frame is the correct behaviour, not just a convenience.
    auto *f = m_glCtx->extraFunctions();
    if (!m_pbo[0])
        f->glGenBuffers(2, m_pbo);
    const qsizetype bytes = qsizetype(size.width()) * size.height() * 4;
    for (int i = 0; i < 2; ++i) {
        f->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[i]);
        f->glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        m_pboFilled[i] = false;
    }
    f->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    m_pboNext = 0;

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

    // ORIENTATION (0.60.6): textures are uploaded TOP-DOWN, verbatim from
    // the QImage — the mirrored() copy that used to live here is gone. It
    // existed to keep every FBO in GL-native bottom-up orientation so that
    // the final fbo->toImage() could do the one flip back; but both the
    // mirror and the toImage flip were full-frame CPU copies, ~2×/frame at
    // 30 fps. The flips CANCEL instead: with top-down uploads, the quad's
    // UV(0,0)-at-bottom-left convention puts the image's TOP row in FBO
    // row 0, every intermediate pass is a 1:1 quad with symmetric kernels
    // (orientation-preserving), and the final raw glReadPixels — which
    // returns FBO row 0 first — hands the rows back in QImage top-down
    // order with no flip at all. background_engine_test (G2) pins this
    // with an asymmetric frame; if a pass is ever added whose kernel is
    // vertically asymmetric, its offsets must be negated to match.
    //
    // The old path also paid a convertToFormat(RGBA8888) on every camera
    // frame (the bridge delivers BGRx-aliased RGB32). Desktop GL 3.3
    // uploads GL_BGRA natively, so the 32-bit formats go up as-is; only
    // exotic formats (none on the production path) pay a conversion.
    QOpenGLPixelTransferOptions opts;
    if (singleChannel) {
        const QImage gray = img.format() == QImage::Format_Grayscale8
            ? img : img.convertToFormat(QImage::Format_Grayscale8);
        opts.setAlignment(1);
        opts.setRowLength(int(gray.bytesPerLine()));   // 1 byte/px ⇒ pixels == bytes
        tex->setData(QOpenGLTexture::Red, QOpenGLTexture::UInt8,
                     gray.constBits(), &opts);
    } else {
        QImage rgba;   // keeps a converted copy alive through setData
        QOpenGLTexture::PixelFormat pixFmt = QOpenGLTexture::BGRA;
        const uchar *bits = img.constBits();
        qsizetype bpl = img.bytesPerLine();
        switch (img.format()) {
        case QImage::Format_RGB32:
        case QImage::Format_ARGB32:
        case QImage::Format_ARGB32_Premultiplied:
            pixFmt = QOpenGLTexture::BGRA;   // 0xAARRGGBB LE = B,G,R,A bytes
            break;
        case QImage::Format_RGBA8888:
        case QImage::Format_RGBA8888_Premultiplied:
        case QImage::Format_RGBX8888:
            pixFmt = QOpenGLTexture::RGBA;
            break;
        default:
            rgba   = img.convertToFormat(QImage::Format_RGBA8888);
            pixFmt = QOpenGLTexture::RGBA;
            bits   = rgba.constBits();
            bpl    = rgba.bytesPerLine();
            break;
        }
        opts.setAlignment(4);                 // 32-bit rows are always 4-aligned
        opts.setRowLength(int(bpl / 4));
        tex->setData(pixFmt, QOpenGLTexture::UInt8, bits, &opts);
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

QImage BackgroundCompositor::takeReadback(QOpenGLFramebufferObject *fbo)
{
    // Pipelined GPU→CPU readback (see the class-comment note). The old
    // fbo->toImage() was a synchronous glReadPixels: the CPU sat parked
    // until the GPU finished all four passes, every frame — 18.8 ms of
    // the 44 ms worker total at 720p on an Iris Xe. With a pixel-pack
    // buffer bound, glReadPixels returns immediately; we map the buffer
    // the GPU filled during the PREVIOUS composite, whose work completed
    // ~a frame period ago, so the map does not wait either.
    auto *f = m_glCtx->extraFunctions();
    const int w = fbo->width(), h = fbo->height();
    const qsizetype bytes = qsizetype(w) * h * 4;

    f->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo->handle());
    f->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[m_pboNext]);
    // GL_BGRA byte order == QImage::Format_RGB32 on little-endian — the
    // BGRx the GStreamer bridge wants back, so no conversion follows.
    // Desktop GL 3.3 core allows BGRA for ReadPixels; this codepath never
    // runs on GLES (the context is created Core 3.3 or init fails).
    f->glReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
    // Flush NOW. An offscreen context never swaps, so nothing else ever
    // submits the command batch — without this the driver is free to sit
    // on the whole frame's render + readback until the NEXT map forces a
    // mega-flush (measured on Iris Xe: ~0.5 s stalls on >5 % of frames,
    // exactly the pathology the PBO exists to avoid). glFlush is a submit,
    // not a wait — by the time we map this buffer a frame later, the GPU
    // has had a full frame period to finish it.
    f->glFlush();
    m_pboFilled[m_pboNext] = true;

    const int prev = 1 - m_pboNext;
    // First composite after init / resize / discardPendingReadback():
    // there is no previous frame parked, so map THIS frame's buffer — a
    // one-off synchronous read that keeps single-shot callers correct.
    // Its flag stays set, so the next call returns this same composite
    // once more (one frame late) and the pipeline is primed from then on.
    const int take = m_pboFilled[prev] ? prev : m_pboNext;

    QImage out(w, h, QImage::Format_RGB32);
    f->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo[take]);
    if (void *p = f->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, bytes,
                                      GL_MAP_READ_BIT)) {
        std::memcpy(out.bits(), p, size_t(bytes));
        f->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
    } else {
        qWarning() << "BackgroundCompositor: PBO map failed — dropping composite";
        out = QImage();
    }
    if (take == prev)
        m_pboFilled[prev] = false;   // consumed; this frame's stays parked
    m_pboNext = prev;

    f->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    f->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return out;
}

void BackgroundCompositor::discardPendingReadback()
{
    // Flag-only: the stale bytes may stay in GPU memory, they just become
    // unreachable — the next takeReadback finds no filled buffer and maps
    // its own frame synchronously. Worker-thread only (same as the
    // composite calls); no GL work, so it is safe even before init.
    m_pboFilled[0] = m_pboFilled[1] = false;
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
    // Failure paths return a NULL image, per this class's documented
    // contract ("Empty QImage on failure"). Until 2026-07-14 they returned
    // `rgba` — the raw camera frame — and relied on every caller noticing
    // via cacheKey that nothing had been composited. In a privacy feature
    // "reveal the input" must not be a return value any caller can miss.
    if (!ensureInitialised()) return {};
    if (rgba.isNull() || mask.isNull()) {
        qWarning() << "BackgroundCompositor::compositeBlur — null input";
        return {};
    }
    if (!m_glCtx->makeCurrent(m_surface)) {
        qWarning() << "BackgroundCompositor::compositeBlur — makeCurrent failed";
        return {};
    }
    if (!ensureFbos(rgba.size())) {
        qWarning() << "BackgroundCompositor::compositeBlur — FBO alloc failed for"
                   << rgba.size();
        return {};
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

    return takeReadback(m_fboOutput);
}

QImage BackgroundCompositor::compositeImage(const QImage &rgba,
                                             const QImage &mask,
                                             const QImage &bg)
{
    // Same fail-closed contract as compositeBlur — null, never `rgba`.
    if (!ensureInitialised()) return {};
    if (rgba.isNull() || mask.isNull() || bg.isNull()) {
        qWarning() << "BackgroundCompositor::compositeImage — null input";
        return {};
    }
    if (!m_glCtx->makeCurrent(m_surface)) {
        qWarning() << "BackgroundCompositor::compositeImage — makeCurrent failed";
        return {};
    }
    if (!ensureFbos(rgba.size())) {
        qWarning() << "BackgroundCompositor::compositeImage — FBO alloc failed for"
                   << rgba.size();
        return {};
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

    return takeReadback(m_fboOutput);
}

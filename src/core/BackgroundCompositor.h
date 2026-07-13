#pragma once

// #20 — Off-screen GL compositor for the background-blur / replace stack.
//
// Owns a hidden QOffscreenSurface + QOpenGLContext + a chain of FBOs that
// implement Talk's 3-stage WebGL2 compositor (joint bilateral mask
// refinement → mask-aware separable Gaussian blur → final blend with
// light wrapping). Constants kept verbatim from
// spreed/src/utils/media/effects/virtual-background/WebGLCompositor.js
// (v23.0.4): SIGMA_SPACE=5, SIGMA_COLOR=0.15, lightWrapping=0.3.
//
// Public API is thread-safe in the sense that the compositor MUST be
// created, used, and destroyed on the SAME thread — the thread where
// the QOpenGLContext lives. BackgroundEngine puts it on its own QThread
// so the streaming thread is never blocked by GL work.
//
// Phase 2a (this commit): API + class skeleton only. The GL context
// initialisation, FBO allocation, and shader compilation land in
// Phase 2b; the actual three-pass render lands in Phase 2c. The header
// is finalised so the BackgroundEngine call sites can be wired today
// without churn later.

#include <QImage>
#include <QObject>
#include <QSize>

class QOpenGLContext;
class QOffscreenSurface;
class QOpenGLFramebufferObject;
class QOpenGLShaderProgram;
class QOpenGLBuffer;
class QOpenGLVertexArrayObject;
class QOpenGLTexture;

class BackgroundCompositor : public QObject
{
    Q_OBJECT

public:
    explicit BackgroundCompositor(QObject *parent = nullptr);
    ~BackgroundCompositor() override;

    // 0.40.1 — set an externally-created QOffscreenSurface (created on
    // the GUI thread, then moved to the worker thread by the owning
    // BackgroundEngine). When set, the compositor uses this surface
    // instead of creating its own; ownership stays with the caller. Must
    // be called before ensureInitialised() / the first composite call.
    void setExternalSurface(QOffscreenSurface *surface);

    // Lazy GL initialisation — called on first compositeBlur() /
    // compositeImage(). Returns false if no GL3.3+ context can be created
    // (caller falls back to pass-through and surfaces the error).
    bool ensureInitialised();

    // Whether ensureInitialised() succeeded at least once.
    bool isReady() const { return m_ready; }

    // Talk's three render modes, kept as separate calls so the engine
    // can pick at frame time without ferrying enums through the GL layer.

    // Blur mode: mask-aware separable Gaussian blur of the frame itself,
    // then composite the (sharp) foreground over the blurred copy using
    // the segmentation mask.
    //   rgba   — input camera frame, premultiplied RGBA
    //   mask   — single-channel 0..255 segmentation mask, same size or
    //            upscaled by the compositor
    //   radius — blur radius in source-resolution pixels; Talk's default
    //            blurValue=10 maps to radius = 10 * width / 720.
    // Returns the composited RGBA. Empty QImage on failure.
    QImage compositeBlur(const QImage &rgba, const QImage &mask, float radius);

    // Image-replace mode. `bg` is the chosen background image, scaled
    // to the source resolution by the caller (so the compositor caches
    // the scaled texture across frames as long as bg.cacheKey is stable).
    QImage compositeImage(const QImage &rgba, const QImage &mask,
                          const QImage &bg);

signals:
    // Fires once if ensureInitialised() fails permanently (no GL 3.3
    // context obtainable). Engine treats this as engineDisabled().
    void initFailed(const QString &reason);

private:
    bool m_ready = false;
    // 0.60.2 (2026-07-13 field RCA) — latched by ensureInitialised() on the
    // first failure so subsequent composite calls return immediately instead
    // of rebuilding a QOpenGLContext (and, pre-fix, leaking a fresh
    // QOffscreenSurface — see releaseAll()) on EVERY FRAME at 15-30 fps.
    // GL 3.3 availability doesn't change mid-session; a fresh compositor
    // instance (background toggled Off→On) naturally resets the latch.
    bool m_initFailedPermanently = false;
    // True when m_surface was supplied via setExternalSurface() and must
    // therefore NOT be deleted in releaseAll() — the engine retains
    // ownership in that case.
    bool m_surfaceExternal = false;

    // GL handles — owned, created in ensureInitialised(), torn down in
    // the destructor. Forward-declared so the header stays light.
    QOpenGLContext             *m_glCtx     = nullptr;
    QOffscreenSurface          *m_surface   = nullptr;
    QOpenGLFramebufferObject   *m_fboInput  = nullptr;   // raw camera
    QOpenGLFramebufferObject   *m_fboMask   = nullptr;   // refined mask alpha
    QOpenGLFramebufferObject   *m_fboBlurH  = nullptr;   // horizontal blur pass
    QOpenGLFramebufferObject   *m_fboBlurV  = nullptr;   // vertical blur pass
    QOpenGLFramebufferObject   *m_fboOutput = nullptr;   // final composite

    // Shader programs — one per fragment stage, all sharing the
    // passthrough.vert. Names mirror the shader filenames.
    QOpenGLShaderProgram *m_progMaskRefine = nullptr;
    QOpenGLShaderProgram *m_progBlurH      = nullptr;
    QOpenGLShaderProgram *m_progBlurV      = nullptr;
    QOpenGLShaderProgram *m_progCompose    = nullptr;

    // Geometry — a single full-screen quad shared by all passes.
    QOpenGLVertexArrayObject *m_vao = nullptr;
    QOpenGLBuffer            *m_vbo = nullptr;

    // Input textures — re-uploaded per frame. Owned + recreated on
    // resolution changes via ensureFbos().
    QOpenGLTexture *m_texFg   = nullptr;   // RGBA camera frame
    QOpenGLTexture *m_texMask = nullptr;   // single-channel mask
    QOpenGLTexture *m_texBg   = nullptr;   // image-mode background (cached)

    QSize m_lastSize;   // FBOs are re-allocated when camera resolution changes

    // Internal helpers split out for testability + symmetry with Talk's
    // WebGLCompositor.js layout.
    bool createContext();
    bool compilePrograms();
    bool createGeometry();
    bool ensureFbos(const QSize &size);
    void uploadTexture(QOpenGLTexture *&tex, const QImage &img, bool singleChannel);
    // runPass: callers are expected to bind the program, set its uniforms,
    // and call runPass(prog, fbo). The function takes care of FBO bind +
    // viewport + clear + draw + cleanup. Doing the uniform setup inside
    // the runPass call site (after prog->bind, before runPass) ensures
    // the program is the active GL program when glUniform* is issued —
    // setting uniforms after release() to glUseProgram(0) would no-op
    // on some drivers (review B1).
    void runPass(QOpenGLShaderProgram *prog, QOpenGLFramebufferObject *target);
    QImage readbackFbo(QOpenGLFramebufferObject *fbo);
    void releaseAll();

    // Identifies the currently-uploaded image-mode background texture so
    // selecting a different image of the same dimensions re-uploads
    // (review B2 — keying on size alone meant changing 1280x720 → other
    // 1280x720 silently used the stale GPU texture).
    qint64 m_texBgCacheKey = 0;
};

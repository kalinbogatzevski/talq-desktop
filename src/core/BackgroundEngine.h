#pragma once

// #20 — Per-frame background blur / replace engine.
//
// Scope: takes RGBA frames from PublishPipeline's shared chain, runs
// person segmentation (TFLite + selfie_segmenter.tflite), composites the
// person over a blurred-self or chosen-image background via the three
// GLSL shaders ported verbatim from Talk's WebGLCompositor.js, and hands
// the composited RGBA back to the encoder branches.
//
// Current state: STUB — pass-through (returns the input frame unchanged).
// The TFLite + GL pipeline lands in Phase 2 / Phase 3 per the plan at
// docs/superpowers/plans/2026-05-24-video-background.md. Until then this
// header exists so the surrounding PublishPipeline wiring can be checked
// in without dragging in a half-finished engine.
//
// Threading: the public API is called from PublishPipeline's appsink
// callback on the GStreamer streaming thread. The engine's interpreter +
// GL context (when wired up) will live on their own QThread so neither
// blocks the streaming thread.

#include <QImage>
#include <QObject>
#include <QString>

class BackgroundCompositor;

class BackgroundEngine : public QObject
{
    Q_OBJECT

public:
    // The four modes mirror Talk's `BACKGROUND_TYPE` enum (constants.ts
    // L411-420). VIDEO / VIDEO_STREAM (Talk's animated backgrounds) are
    // explicitly out of scope; the enum value is reserved so a future
    // session can add them without renumbering.
    enum class Mode {
        None = 0,
        Blur = 1,
        Image = 2,
        Video = 3,         // reserved
        VideoStream = 4,   // reserved
    };
    Q_ENUM(Mode)

    explicit BackgroundEngine(QObject *parent = nullptr);
    ~BackgroundEngine() override;

    // Settings — live-applied. Changing while a frame is in flight is safe
    // (the in-flight frame uses the prior values, the next frame picks up
    // the new ones).
    void setMode(Mode m);
    Mode mode() const;

    // 1..20, matches Talk's blurStrength. The scaling onto Talk's GLSL
    // `backgroundBlurValue = strength * width/720` happens inside the
    // engine, NOT here.
    void setBlurStrength(int strength);
    int  blurStrength() const;

    // For Image mode. Empty path = fall back to None (matches Talk's
    // behaviour when an image is missing).
    void setImagePath(const QString &absolutePath);
    QString imagePath() const;

    // Per-frame entry point. Returns the composited RGBA QImage. For the
    // stub: returns input unchanged. The frame is always shaped to the
    // upstream caps (RGBA, premultiplied alpha).
    QImage processFrame(const QImage &rgba);

    // True when a working inference backend was constructed (TFLite + GL).
    // In the stub: always false (no backend yet).
    bool isReady() const;

signals:
    // Fires when the engine permanently disables itself for this session
    // (e.g. inference fails repeatedly). The UI should toast the reason
    // and flip the QSetting back to None.
    void engineDisabled(const QString &reason);

    // Fires once when an Image-mode background fails to load. UI surfaces
    // a toast; per-frame qWarnings are then suppressed for that path
    // until setImagePath() changes the source. Per the project rule
    // "never hide errors silently" — surface once, then go quiet.
    void backgroundImageFailed(const QString &path);

private:
    Mode    m_mode = Mode::None;
    int     m_blurStrength = 10;   // Talk's default
    QString m_imagePath;

    // Owned in Phase 2a as a forward-declared pointer; the engine
    // constructs the compositor lazily on the first non-None mode so a
    // None-mode publisher pays zero startup cost. Lifetime is managed
    // by Qt's parent-child ownership (parent = this engine); we do NOT
    // delete this in the dtor (would double-free).
    BackgroundCompositor *m_compositor = nullptr;

    // Image-mode caches — per Phase 2d.1 review, the disk read +
    // SmoothTransformation scale would have happened EVERY frame
    // (5-50 ms blocking at 30 fps on a slow share). Cache by path
    // (invalidated in setImagePath) and by scaled size.
    QImage  m_cachedBgRaw;          // last successful disk load
    QString m_cachedBgPath;
    QImage  m_cachedBgScaled;       // last scaled-to-frame image
    QSize   m_cachedBgScaledSize;
    bool    m_cachedBgFailed = false;   // suppress per-frame qWarning loop
};

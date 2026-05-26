#pragma once

// #20 follow-up - live camera preview for the Settings → Background
// section. Lets the user see the effect of Blur / Image / Strength
// changes on their own face WITHOUT having to join a call first.
//
// Design:
// - Opens its own minimal GStreamer chain (mfvideosrc preferred,
//   ksvideosrc fallback - matches PublishPipeline's selection) at a
//   small resolution (640×360) and modest framerate (15 fps). The
//   chain is intentionally lighter than the publisher's; this is a
//   preview, not a call.
// - Each pulled BGRx frame is routed through the BackgroundEngine
//   pointer supplied by the dialog. When the engine's mode is None
//   the frames pass through unchanged; when Blur / Image is active the
//   compositor runs and the preview reflects whatever the user just
//   tweaked.
// - Tear down on stop() OR on destruction so the camera is released
//   before any call attempts to claim it.
//
// Threading: the appsink "new-sample" handler runs on a GStreamer
// streaming thread. It hops the captured QImage to the Qt thread via
// a queued signal, identical pattern to PublishPipeline's preview
// branch. The engine runs on whichever thread its parent lives on
// (typically the Qt main thread); the cross-thread hop is therefore
// implicit through the queued imageReady connection.

#include <QObject>
#include <QImage>
#include <QPointer>
#include <QString>

#include <gst/gst.h>

class BackgroundEngine;

class BgPreviewSource : public QObject
{
    Q_OBJECT

public:
    explicit BgPreviewSource(BackgroundEngine *engine, QObject *parent = nullptr);
    ~BgPreviewSource() override;

    // Open the system default camera (mfvideosrc preferred) and start
    // emitting imageReady at ~15 fps. Returns false if no camera could
    // be opened OR the chain failed to PLAY. On failure imageReady
    // never fires; the caller should display a placeholder.
    bool start();
    void stop();
    bool isRunning() const { return m_running; }

signals:
    // Already engine-processed (or pass-through if engine == nullptr /
    // Mode::None). Format_RGB32; size 640×360 (preview resolution).
    void imageReady(const QImage &img);

    // Fires once on a fatal startup error (camera busy, no source
    // factory, link failure). The dialog uses this to swap the
    // preview label for a placeholder.
    void unavailable(const QString &reason);

private:
    static GstFlowReturn onNewSample(GstElement *appsink, gpointer userData);

    // The engine is owned by the dialog and parented to it; Qt's
    // child-destruction order is unspecified, so an in-flight queued
    // lambda on the Qt thread might fire after the engine is gone.
    // QPointer null-checks at lambda dispatch time so we never
    // dereference a freed engine. Source is also parented to the
    // dialog so the QPointer<BgPreviewSource> guard handles the
    // post-self-destruction case independently.
    QPointer<BackgroundEngine> m_engine;
    GstElement       *m_pipeline = nullptr;
    GstElement       *m_source   = nullptr;
    GstElement       *m_appsink  = nullptr;
    bool              m_running  = false;
};

#include "core/ScreenSharePipeline.h"
#include "core/VideoEncoderUtil.h"
#include <gst/rtp/rtp.h>
#include <gst/app/app.h>
#include <QPointer>
#include <QApplication>
#include <QDebug>
#include <QPointer>
#include <QRegularExpression>
#include <QScreen>
#include <QUrl>

#ifdef Q_OS_WIN
#include <windows.h>   // MonitorFromPoint / HMONITOR / HWND
#endif

ScreenSharePipeline::ScreenSharePipeline(QObject *parent)
    : QObject(parent)
    , m_previewProvider(new VideoFrameProvider(this))
{
    // Negotiation watchdog — 10 s from start() to "subscriber-side ICE
    // connected". If we don't get there the share has silently failed
    // (#134 "stream doesn't always start" pattern: SDP exchanged but the
    // wire never carries decodable frames). Surface a clear error so the
    // UI shows a failure instead of an apparently-active share that's
    // dead on the wire.
    m_startWatchdog.setSingleShot(true);
    m_startWatchdog.setInterval(10000);
    connect(&m_startWatchdog, &QTimer::timeout, this, [this]() {
        if (m_iceReachedConnected) return;
        qWarning() << "ScreenSharePipeline: start watchdog fired — "
                      "ICE didn't reach connected within 10 s. "
                      "Emitting error so CallManager tears down + the UI "
                      "shows a clear failure.";
        emit error(QStringLiteral(
            "Screen sharing didn't start (ICE never connected). "
            "Try sharing again."));
    });

    // Capture-frame watchdog (#2): 6 s from start() to the first captured
    // frame. d3d11screencapturesrc (WGC) can connect ICE but never emit a
    // buffer if it failed to attach to the window; this catches that
    // distinct failure mode (silent "Starting remote screen share…"
    // forever on the receiver). Cleared on the first frame via the pad
    // probe below.
    m_frameWatchdog.setSingleShot(true);
    m_frameWatchdog.setInterval(6000);
    connect(&m_frameWatchdog, &QTimer::timeout, this, [this]() {
        if (m_firstFrameSeen.load()) return;
        qWarning() << "ScreenSharePipeline: capture produced NO frames within "
                      "6 s — the capture source failed to attach to the "
                      "target. Surfacing error for retry.";
        emit error(QStringLiteral(
            "Screen sharing didn't start (no frames captured). "
            "Try sharing again, or pick a different window/screen."));
    });
}

GstPadProbeReturn ScreenSharePipeline::onCaptureBuffer(GstPad *, GstPadProbeInfo *,
                                                       gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    if (self && !self->m_firstFrameSeen.exchange(true)) {
        qInfo() << "ScreenSharePipeline: first captured frame seen — "
                   "capture source attached OK";
    }
    return GST_PAD_PROBE_OK;
}

ScreenSharePipeline::~ScreenSharePipeline()
{
    stop();
}

bool ScreenSharePipeline::start(const QString &stunServer, const QList<TurnServer> &turnServers,
                                int monitorIndex, quintptr windowHandle)
{
    if (m_running) return false;

    m_pipeline = gst_pipeline_new(nullptr);
    m_webrtcbin = gst_element_factory_make("webrtcbin", nullptr);

    if (!m_pipeline || !m_webrtcbin) {
        emit error("Failed to create screen share pipeline elements");
        cleanup();
        return false;
    }

    if (!stunServer.isEmpty()) {
        QString gstStun = stunServer;
        if (gstStun.startsWith("stun:") && !gstStun.startsWith("stun://"))
            gstStun.replace("stun:", "stun://");
        g_object_set(m_webrtcbin, "stun-server", gstStun.toUtf8().constData(), nullptr);
    }
    g_object_set(m_webrtcbin, "bundle-policy",
                 GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);

    for (const auto &turn : turnServers) {
        for (const auto &url : turn.urls) {
            QString gstUrl = url;
            gstUrl.remove(QRegularExpression("\\?transport=.*$"));
            if (gstUrl.startsWith("turn:") && !gstUrl.startsWith("turn://"))
                gstUrl.replace("turn:", "turn://");
            if (gstUrl.startsWith("turns:") && !gstUrl.startsWith("turns://"))
                gstUrl.replace("turns:", "turns://");
            QString escapedUser = QString(QUrl::toPercentEncoding(turn.username));
            QString escapedCred = QString(QUrl::toPercentEncoding(turn.credential));
            gstUrl.replace("://", QString("://%1:%2@").arg(escapedUser, escapedCred));
            gboolean ret = FALSE;
            g_signal_emit_by_name(m_webrtcbin, "add-turn-server", gstUrl.toUtf8().constData(), &ret);
        }
    }

    // Screen capture source. Two real Windows bugs the previous wiring hit:
    //
    //  - Window capture: `d3d11screencapturesrc` only honors `window-handle`
    //    when `capture-api=1` (Windows Graphics Capture). In the default
    //    DXGI mode it captures a monitor and silently ignores the HWND →
    //    the picker said "this window", the peer saw a monitor.
    //
    //  - Monitor capture: `dx9screencapsrc`'s `monitor` is the DXGI output
    //    index, which does NOT match `QApplication::screens()` order on
    //    multi-monitor Windows → picking "Screen 2" could share Screen 1.
    //    Target by HMONITOR instead (stable physical identifier), derived
    //    from the chosen Qt screen's geometry via `MonitorFromPoint`.
    GstElement *screenSrc = nullptr;

    // Harness override: synthetic capture for talq-call-test (no real desktop
    // session, no HWND). videotestsrc emits a bouncing-ball pattern that
    // changes frame-to-frame, so the wire payload is encodable, decodable,
    // and visually distinct from a frozen black frame on the receiver side.
    // The pipeline graph downstream of the source is identical to a real
    // desktop capture.
    if (qEnvironmentVariableIsSet("TALQ_SS_TESTSRC")) {
        screenSrc = gst_element_factory_make("videotestsrc", nullptr);
        if (screenSrc) {
            g_object_set(screenSrc,
                         "is-live", TRUE,
                         "pattern", 18 /* ball — moves frame-to-frame */,
                         nullptr);
            qInfo() << "ScreenSharePipeline: TALQ_SS_TESTSRC=1 — synthetic "
                       "videotestsrc replacing desktop capture";
        }
    }

#ifdef Q_OS_WIN
    if (!screenSrc && windowHandle != 0) {
        // Property-set order matters for d3d11screencapturesrc: capture-api
        // must be configured BEFORE window-handle, otherwise the element
        // may auto-resolve a monitor target from the (still-default)
        // capture-api mode and ignore the late window-handle. Set them
        // in two calls to make the order explicit.
        screenSrc = gst_element_factory_make("d3d11screencapturesrc", nullptr);
        if (screenSrc) {
            g_object_set(screenSrc, "capture-api", 1 /* WGC */, nullptr);
            g_object_set(screenSrc, "window-handle", (guint64)windowHandle,
                                    "show-cursor", TRUE, nullptr);
            // Read back to confirm the property actually took (#134
            // wrong-window diag): if the readback differs from what we
            // asked for, d3d11screencapturesrc rejected the HWND silently.
            guint64 readBack = 0;
            g_object_get(screenSrc, "window-handle", &readBack, nullptr);
            qInfo().nospace() << "ScreenSharePipeline: window capture (WGC) "
                              << "requested hwnd=" << (quintptr)windowHandle
                              << " readback=" << (quintptr)readBack
                              << ((quintptr)readBack == (quintptr)windowHandle
                                  ? " ✓"
                                  : " ✗ MISMATCH (capture src ignored our pick)");
        }
    }

    if (!screenSrc) {
        const auto screens = QApplication::screens();
        HMONITOR hMon = nullptr;
        if (monitorIndex >= 0 && monitorIndex < screens.size()) {
            const QPoint tl = screens[monitorIndex]->geometry().topLeft();
            // +1,+1 to avoid the edge case where a top-left pixel sits on
            // the boundary between two monitors.
            POINT p{ tl.x() + 1, tl.y() + 1 };
            hMon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
        }
        if (hMon) {
            screenSrc = gst_element_factory_make("d3d11screencapturesrc", nullptr);
            if (screenSrc) {
                g_object_set(screenSrc,
                             "monitor-handle",
                             (guint64)(quintptr)hMon,
                             "show-cursor", TRUE, nullptr);
                qInfo() << "ScreenSharePipeline: monitor capture via HMONITOR"
                        << (quintptr)hMon
                        << "(Qt screen index" << monitorIndex << ")";
            }
        }
    }
#endif

    if (!screenSrc) {
        // d3d11screencapturesrc unavailable / refused to construct. The
        // older fallbacks (dx9screencapsrc, gdiscreencapsrc) interpret
        // `monitor` as a DXGI index that does NOT match QApplication
        // ordering, and neither of them can honor a window-handle — so
        // a window-share request would silently turn into a monitor
        // capture of the wrong screen (a field report: "sharing one and the
        // same display no matter what is selected", #134). We surface
        // a clear error instead. If d3d11screencapturesrc is the only
        // path that reliably honors the user's pick, missing it is a
        // deploy problem we need to know about.
        const QString why = (windowHandle != 0)
            ? "Window capture requires gstd3d11 (Windows Graphics "
              "Capture). It is not available in this TalQ build. "
              "Window sharing is unavailable."
            : "Monitor capture requires gstd3d11 (HMONITOR-targeted). "
              "It is not available in this TalQ build, and the legacy "
              "fallbacks would share the wrong display.";
        qWarning() << "ScreenSharePipeline:" << why;
        emit error(why);
        cleanup();
        return false;
    }

    // Chain: screenSrc -> queue(leaky) -> videoconvert -> videoscale ->
    //        scaleCaps(<= m_capW x m_capH) -> encoder -> [h264parse] ->
    //        pay -> capsfilter(ssrc). Capture is downscaled to a cap
    //        (default 1080p) BEFORE encode: a native 4K raw frame is
    //        ~38 MB and the unbounded native-res capture/convert/encoder
    //        pools ballooned RAM ~400 MB the instant sharing started (and
    //        forced real-time 4K H264). The cap is settable via
    //        setQualityCap() so the quality switch can stop()->start() at
    //        a higher resolution on demand. The leaky queue bounds the
    //        raw-frame backlog. Hardware H264 preferred, software VP8
    //        last-resort fallback.
    GstElement *capQueue     = gst_element_factory_make("queue", nullptr);
    GstElement *vscale       = gst_element_factory_make("videoscale", nullptr);
    GstElement *scaleCaps    = gst_element_factory_make("capsfilter", nullptr);
    GstElement *videoConvert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *ssrcFilter   = gst_element_factory_make("capsfilter", nullptr);
    // 0.41.1-beta — self-preview tee. After the encode-side downscale
    // (scaleCaps) we split into the encoder branch (existing) + a
    // preview branch (queue→convert→appsink). The preview frames feed
    // m_previewProvider so the user sees a live thumbnail of what
    // they're actually broadcasting. Construct-tolerant: if any of
    // these factory_make() returns nullptr the share still works,
    // just without the self-preview tile.
    GstElement *previewTee     = gst_element_factory_make("tee",          nullptr);
    GstElement *previewQueue   = gst_element_factory_make("queue",        nullptr);
    GstElement *previewConvert = gst_element_factory_make("videoconvert", nullptr);
    m_previewAppsink           = gst_element_factory_make("appsink",      "share-preview-sink");
    bool useH264 = false;
    GstElement *venc = makeWebrtcVideoEncoder(/*screen=*/true, m_initBitrate,
                                              &useH264, &m_videoParser,
                                              &m_encoderDesc);
    GstElement *pay = venc ? gst_element_factory_make(
                                 useH264 ? "rtph264pay" : "rtpvp8pay", nullptr)
                           : nullptr;

    if (!videoConvert || !venc || !pay || !ssrcFilter
        || !capQueue || !vscale || !scaleCaps) {
        emit error("Failed to create screen share encoding elements");
        // Not bin-added yet → drop floating refs (cleanup() only nulls).
        for (GstElement *e : { capQueue, vscale, scaleCaps, videoConvert,
                               venc, m_videoParser, pay, ssrcFilter })
            if (e) gst_object_unref(e);
        m_videoParser = nullptr;
        cleanup();
        return false;
    }
    // Bound the raw-frame backlog (4K BGRA ≈ 38 MB/buffer) and cap the
    // capture resolution before encode. Range caps + fixed PAR let a
    // smaller screen pass through untouched while a 4K screen scales down
    // to fit the cap, aspect preserved.
    g_object_set(capQueue, "leaky", 2 /* downstream */,
                 "max-size-buffers", 3, "max-size-bytes", (guint)0,
                 "max-size-time", (guint64)0, nullptr);
    {
        const QString capStr = QStringLiteral(
            "video/x-raw,width=(int)[2,%1],height=(int)[2,%2],"
            "pixel-aspect-ratio=1/1").arg(m_capW).arg(m_capH);
        GstCaps *cap = gst_caps_from_string(capStr.toUtf8().constData());
        g_object_set(scaleCaps, "caps", cap, nullptr);
        gst_caps_unref(cap);
        qInfo().nospace() << "ScreenSharePipeline: capture capped to "
                          << m_capW << "x" << m_capH << " before encode";
    }

    qDebug().nospace() << "ScreenSharePipeline: encoder = " << m_encoderDesc;
    m_videoEncoder = venc;     // for rtpgccbwe live bitrate
    m_useH264 = useH264;

    if (useH264) {
        // Repeat SPS/PPS before every IDR so subscribers that join the SFU
        // after the first keyframe still decode; zero-latency aggregation.
        g_object_set(m_videoParser, "config-interval", -1, nullptr);
        g_object_set(pay, "aggregate-mode", 1 /* zero-latency */,
                     "config-interval", -1, nullptr);
    }

    // SSRC capsfilter
    guint32 videoSsrc = g_random_int();
    g_object_set(pay, "ssrc", videoSsrc, "pt", 96, nullptr);
    // TWCC seq numbers on the wire, same id as codec-preferences below.
    // Only advertise extmap in SDP if this actually succeeds (SDP/wire
    // must agree or GCC starves).
    bool twccActive = false;
    {
        GstRTPHeaderExtension *twcc =
            gst_rtp_header_extension_create_from_uri(kTwccUri);
        if (twcc) {
            gst_rtp_header_extension_set_id(twcc, kTwccExtId);
            g_signal_emit_by_name(pay, "add-extension", twcc);
            gst_object_unref(twcc);
            twccActive = true;
            qDebug() << "ScreenSharePipeline: TWCC ext id" << kTwccExtId
                     << "added to payloader";
        } else {
            qWarning() << "ScreenSharePipeline: rtphdrexttwcc unavailable — "
                          "send-side adaptive bitrate disabled";
        }
    }
    {
        GstCaps *ssrcCaps = gst_caps_from_string("application/x-rtp");
        gst_caps_set_simple(ssrcCaps, "ssrc", G_TYPE_UINT, videoSsrc, nullptr);
        g_object_set(ssrcFilter, "caps", ssrcCaps, nullptr);
        gst_caps_unref(ssrcCaps);
    }

    // 0.41.1-beta — preview branch is best-effort. Construct only if all
    // four factories succeeded; otherwise fall back to the un-teed chain
    // so the share itself never regresses on a missing-element install.
    const bool previewOK = (previewTee && previewQueue
                             && previewConvert && m_previewAppsink);

    if (previewOK) {
        gst_bin_add_many(GST_BIN(m_pipeline), screenSrc, capQueue, videoConvert,
                         vscale, scaleCaps, previewTee,
                         previewQueue, previewConvert, m_previewAppsink,
                         venc, pay, ssrcFilter, m_webrtcbin, nullptr);
    } else {
        if (previewTee)     gst_object_unref(previewTee);
        if (previewQueue)   gst_object_unref(previewQueue);
        if (previewConvert) gst_object_unref(previewConvert);
        if (m_previewAppsink) { gst_object_unref(m_previewAppsink); m_previewAppsink = nullptr; }
        gst_bin_add_many(GST_BIN(m_pipeline), screenSrc, capQueue, videoConvert,
                         vscale, scaleCaps, venc, pay, ssrcFilter, m_webrtcbin,
                         nullptr);
    }
    if (m_videoParser)
        gst_bin_add(GST_BIN(m_pipeline), m_videoParser);

    qDebug() << "ScreenSharePipeline: elements added to pipeline (preview="
             << (previewOK ? "ON" : "OFF") << ")";

    gboolean linked;
    if (previewOK) {
        // BGRx on the preview branch — same format VideoFrameProvider
        // consumes elsewhere; sync=FALSE so the share isn't gated on
        // the preview branch keeping up.
        {
            GstCaps *bgrx = gst_caps_from_string("video/x-raw,format=BGRx");
            g_object_set(m_previewAppsink,
                         "caps",          bgrx,
                         "emit-signals",  TRUE,
                         "sync",          FALSE,
                         "max-buffers",   (guint)2,
                         "drop",          TRUE,
                         nullptr);
            gst_caps_unref(bgrx);
        }
        g_object_set(previewQueue,
                     "leaky", 2 /* downstream */,
                     "max-size-buffers", 2u,
                     "max-size-bytes",   (guint)0,
                     "max-size-time",    (guint64)0,
                     nullptr);
        g_signal_connect(m_previewAppsink, "new-sample",
                         G_CALLBACK(onPreviewSample), this);
        linked = gst_element_link_many(screenSrc, capQueue, videoConvert,
                                       vscale, scaleCaps, previewTee, nullptr);
        linked = linked && gst_element_link(previewTee, venc);
        linked = linked && gst_element_link_many(
                                previewTee, previewQueue,
                                previewConvert, m_previewAppsink, nullptr);
    } else {
        linked = gst_element_link_many(screenSrc, capQueue, videoConvert,
                                       vscale, scaleCaps, venc, nullptr);
    }
    if (m_videoParser)
        linked = linked && gst_element_link_many(venc, m_videoParser, pay,
                                                 ssrcFilter, nullptr);
    else
        linked = linked && gst_element_link_many(venc, pay,
                                                 ssrcFilter, nullptr);
    if (!linked) {
        emit error("Failed to link screen share chain");
        cleanup();
        return false;
    }

    qDebug() << "ScreenSharePipeline: chain linked";

    // Frame-counter probe on the capture src's output pad (#2). Fires the
    // first time a buffer leaves the screen capture element — clears the
    // frame watchdog. If no buffer ever flows (WGC failed to attach), the
    // watchdog fires instead.
    {
        GstPad *srcPad = gst_element_get_static_pad(screenSrc, "src");
        if (srcPad) {
            gst_pad_add_probe(srcPad, GST_PAD_PROBE_TYPE_BUFFER,
                              &ScreenSharePipeline::onCaptureBuffer, this, nullptr);
            gst_object_unref(srcPad);
        }
    }

    // Link to webrtcbin
    GstPad *ssrcSrcPad = gst_element_get_static_pad(ssrcFilter, "src");
    GstPad *sinkPad = gst_element_request_pad_simple(m_webrtcbin, "sink_%u");
    gst_pad_link(ssrcSrcPad, sinkPad);

    // Set transceiver sendonly, codec matching the chosen encoder
    GstWebRTCRTPTransceiver *vt = nullptr;
    g_object_get(sinkPad, "transceiver", &vt, nullptr);
    if (vt) {
        GstCaps *vc = gst_caps_from_string(
            useH264
              ? "application/x-rtp,media=video,encoding-name=H264,"
                "clock-rate=90000,payload=96"
              : "application/x-rtp,media=video,encoding-name=VP8,"
                "clock-rate=90000,payload=96");
        // webrtcbin builds a=extmap from these caps, not the payloader —
        // put TWCC here so Janus negotiates transport-wide-cc and feeds
        // GCC. Only if the payloader actually writes it (twccActive).
        if (twccActive) {
            char extField[16];
            g_snprintf(extField, sizeof(extField), "extmap-%d", kTwccExtId);
            gst_caps_set_simple(vc, extField, G_TYPE_STRING, kTwccUri, nullptr);
        }
        g_object_set(vt, "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY,
                     "codec-preferences", vc, nullptr);
        gst_caps_unref(vc);
        gst_object_unref(vt);
    }
    gst_object_unref(ssrcSrcPad);
    gst_object_unref(sinkPad);

    // Data channel — Janus requires at least one for publisher registration
    {
        GstWebRTCDataChannel *dc = nullptr;
        g_signal_emit_by_name(m_webrtcbin, "create-data-channel", "status", nullptr, &dc);
        if (dc) {
            qDebug() << "ScreenSharePipeline: created data channel 'status'";
            g_object_unref(dc);
        }
    }

    // Signals
    g_signal_connect(m_webrtcbin, "on-negotiation-needed",
                     G_CALLBACK(onNegotiationNeeded), this);
    g_signal_connect(m_webrtcbin, "on-ice-candidate",
                     G_CALLBACK(onIceCandidate), this);
    g_signal_connect(m_webrtcbin, "notify::ice-connection-state",
                     G_CALLBACK(onIceStateChanged), this);
    g_signal_connect(m_webrtcbin, "notify::ice-gathering-state",
                     G_CALLBACK(onIceGatheringStateChanged), this);
    g_signal_connect(m_webrtcbin, "request-aux-sender",
                     G_CALLBACK(onRequestAuxSender), this);

    qDebug() << "ScreenSharePipeline: setting pipeline to PLAYING...";
    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    qDebug() << "ScreenSharePipeline: set_state returned" << ret;
    if (ret == GST_STATE_CHANGE_FAILURE) {
        emit error("Failed to start screen share pipeline");
        cleanup();
        return false;
    }

    m_running = true;
    m_iceReachedConnected = false;
    m_firstFrameSeen.store(false);
    m_startWatchdog.start();
    m_frameWatchdog.start();
    qDebug() << "ScreenSharePipeline: started, capturing primary monitor "
                "(10 s ICE + 6 s frame watchdogs armed)";
    return true;
}

void ScreenSharePipeline::stop()
{
    if (!m_running) return;
    m_startWatchdog.stop();
    m_frameWatchdog.stop();
    cleanup();
    m_running = false;
    qDebug() << "ScreenSharePipeline: stopped";
}

void ScreenSharePipeline::cleanup()
{
    m_shuttingDown.store(true);
    m_startWatchdog.stop();
    m_frameWatchdog.stop();
    if (m_webrtcbin)
        g_signal_handlers_disconnect_by_data(m_webrtcbin, this);
    if (m_gccbwe)  // notify::estimated-bitrate is on the gcc element
        g_signal_handlers_disconnect_by_data(m_gccbwe, this);
    // 0.41.1-beta — disconnect preview appsink callback before pipeline
    // goes to NULL so a streaming-thread sample can't race the teardown.
    if (m_previewAppsink)
        g_signal_handlers_disconnect_by_data(m_previewAppsink, this);
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_webrtcbin = nullptr;
    m_videoEncoder = nullptr;  // owned by the (now-freed) pipeline
    m_gccbwe = nullptr;        // owned by webrtcbin (now-freed)
    m_videoParser = nullptr;   // owned by the (now-freed) pipeline
    m_previewAppsink = nullptr;
    m_useH264 = false;
    m_remoteDescSet = false;
    m_pendingCandidates.clear();
}

GstFlowReturn ScreenSharePipeline::onPreviewSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;
    // Hand the sample to VideoFrameProvider on the Qt thread — same
    // pattern as PublishPipeline::onPreviewSample. feedFrame owns the
    // unref via the queued lambda's capture.
    QPointer<ScreenSharePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, sample]() {
        if (guard && guard->m_previewProvider)
            guard->m_previewProvider->feedFrame(sample);
        gst_sample_unref(sample);
    }, Qt::QueuedConnection);
    return GST_FLOW_OK;
}

void ScreenSharePipeline::setRemoteAnswer(const QString &sdp)
{
    if (!m_webrtcbin) return;

    QByteArray sdpUtf8 = sdp.toUtf8();
    GstSDPMessage *sdpMsg;
    gst_sdp_message_new(&sdpMsg);
    gst_sdp_message_parse_buffer((const guint8 *)sdpUtf8.constData(), sdpUtf8.size(), sdpMsg);

    GstWebRTCSessionDescription *desc = gst_webrtc_session_description_new(
        GST_WEBRTC_SDP_TYPE_ANSWER, sdpMsg);
    g_signal_emit_by_name(m_webrtcbin, "set-remote-description", desc, nullptr);
    gst_webrtc_session_description_free(desc);

    m_remoteDescSet = true;
    for (const auto &c : m_pendingCandidates)
        g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate", c.first, c.second.toUtf8().constData());
    m_pendingCandidates.clear();
    qDebug() << "ScreenSharePipeline: set remote answer";
}

void ScreenSharePipeline::addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid)
{
    Q_UNUSED(sdpMid)
    if (!m_webrtcbin) return;
    if (!m_remoteDescSet) {
        m_pendingCandidates.append({sdpMLineIndex, candidate});
        return;
    }
    g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate",
                          sdpMLineIndex, candidate.toUtf8().constData());
}

void ScreenSharePipeline::pollBus()
{
    if (!m_pipeline) return;
    GstBus *bus = gst_element_get_bus(m_pipeline);
    GstMessage *msg;
    while ((msg = gst_bus_pop(bus)) != nullptr) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr; gchar *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            qWarning() << "ScreenSharePipeline ERROR:" << err->message;
            g_clear_error(&err); g_free(dbg);
        }
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
}

// --- Static callbacks ---

void ScreenSharePipeline::onNegotiationNeeded(GstElement *webrtc, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    GstPromise *promise = gst_promise_new_with_change_func(onOfferCreated, self, nullptr);
    g_signal_emit_by_name(webrtc, "create-offer", nullptr, promise);
}

void ScreenSharePipeline::onOfferCreated(GstPromise *promise, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);

    if (!offer || !self->m_webrtcbin) {
        if (offer) gst_webrtc_session_description_free(offer);
        gst_promise_unref(promise);
        return;
    }

    g_signal_emit_by_name(self->m_webrtcbin, "set-local-description", offer, nullptr);

    gchar *sdpText = gst_sdp_message_as_text(offer->sdp);
    QString sdp = QString::fromUtf8(sdpText);
    g_free(sdpText);
    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);

    QPointer<ScreenSharePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, sdp]() {
        if (!guard) return;
        emit guard->localOfferReady(sdp);
    }, Qt::QueuedConnection);
}

void ScreenSharePipeline::onIceCandidate(GstElement *, guint mlineIndex, gchar *candidate, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    QString c = QString::fromUtf8(candidate);
    int ml = static_cast<int>(mlineIndex);
    QPointer<ScreenSharePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, c, ml]() {
        if (!guard) return;
        emit guard->iceCandidateReady(c, ml, QString("0"));
    }, Qt::QueuedConnection);
}

void ScreenSharePipeline::onIceStateChanged(GObject *obj, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    GstWebRTCICEConnectionState state;
    g_object_get(obj, "ice-connection-state", &state, nullptr);
    const char *names[] = {"new", "checking", "connected", "completed", "failed", "disconnected", "closed"};
    int idx = static_cast<int>(state);
    QString stateName = (idx < 7) ? names[idx] : "unknown";
    // Always log the ICE transition — #10 debug: when screen-share "doesn't
    // always start", the smoking-gun is which ICE state never advances.
    qInfo().nospace() << "ScreenSharePipeline: ICE state -> " << stateName;
    QPointer<ScreenSharePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, stateName]() {
        if (!guard) return;
        if (stateName == QLatin1String("connected") ||
            stateName == QLatin1String("completed")) {
            guard->m_iceReachedConnected = true;
            guard->m_startWatchdog.stop();
        }
        emit guard->iceStateChanged(stateName);
    }, Qt::QueuedConnection);
}

void ScreenSharePipeline::onIceGatheringStateChanged(GObject *obj, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    GstWebRTCICEGatheringState state;
    g_object_get(obj, "ice-gathering-state", &state, nullptr);
    if (state != GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) return;
    QPointer<ScreenSharePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard]() {
        if (!guard) return;
        qDebug() << "ScreenSharePipeline: ICE gathering complete";
        emit guard->iceGatheringComplete();
    }, Qt::QueuedConnection);
}

GstElement *ScreenSharePipeline::onRequestAuxSender(GstElement *, GObject *,
                                                    gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    if (self->m_shuttingDown.load()) return nullptr;
    GstElement *gcc = gst_element_factory_make("rtpgccbwe", nullptr);
    if (!gcc) {
        qWarning() << "ScreenSharePipeline: rtpgccbwe unavailable — encoder "
                      "stays at fixed bitrate";
        return nullptr;
    }
    // Screen is VBR: GCC moves the sustained average; the encoder keeps its
    // configured peak headroom for bursts (scrolling, motion). Floor low so
    // a near-static screen costs almost nothing; ceiling = server screen cap.
    g_object_set(gcc,
                 "min-bitrate", (guint)500000,
                 "max-bitrate", (guint)self->m_maxBitrate,
                 "estimated-bitrate", (guint)self->m_initBitrate,
                 nullptr);
    g_signal_connect(gcc, "notify::estimated-bitrate",
                     G_CALLBACK(onGccBitrate), self);
    self->m_gccbwe = gcc;
    qInfo().nospace() << "ScreenSharePipeline: rtpgccbwe attached (min 500k, "
                         "max " << self->m_maxBitrate << ", start "
                      << self->m_initBitrate << ")";
    return gcc;  // webrtcbin takes ownership
}

void ScreenSharePipeline::onGccBitrate(GObject *gcc, GParamSpec *,
                                       gpointer userData)
{
    auto *self = static_cast<ScreenSharePipeline *>(userData);
    if (self->m_shuttingDown.load() || !self->m_videoEncoder) return;
    guint est = 0;
    g_object_get(gcc, "estimated-bitrate", &est, nullptr);
    if (est == 0) return;
    if (est > (guint)self->m_maxBitrate) est = (guint)self->m_maxBitrate;
    // Deadband (see PublishPipeline::onGccBitrate) — spare the hardware
    // encoder a reconfigure storm it mostly rejects.
    const int last = self->m_lastAppliedBitrate;
    if (last != 0) {
        const guint delta = est > (guint)last ? est - last : (guint)last - est;
        if (delta * 100u < (guint)last * 15u) return;
    }
    self->m_lastAppliedBitrate = (int)est;
    setWebrtcVideoEncoderBitrate(self->m_videoEncoder, self->m_useH264, est);
    qInfo().nospace() << "ScreenSharePipeline: GCC -> encoder "
                      << (est / 1000) << " kbps";
}

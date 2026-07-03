#include "core/SubscribePipeline.h"
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QStringList>
#include <QUrl>
#include <thread>

SubscribePipeline::SubscribePipeline(const QString &remoteSessionId, QObject *parent)
    : QObject(parent)
    , m_remoteSessionId(remoteSessionId)
{
    m_videoProvider = new VideoFrameProvider(this);
}

SubscribePipeline::~SubscribePipeline()
{
    stop();
}

bool SubscribePipeline::start(const QString &stunServer, const QList<TurnServer> &turnServers,
                              const QString &audioOutputDeviceId)
{
    if (m_running) return false;
    m_audioOutputDeviceId = audioOutputDeviceId;

    m_pipeline = gst_pipeline_new(nullptr);
    m_webrtcbin = gst_element_factory_make("webrtcbin", nullptr);

    if (!m_pipeline || !m_webrtcbin) {
        emit error("Failed to create subscribe pipeline elements");
        cleanup();
        return false;
    }

    if (!stunServer.isEmpty()) {
        // Nextcloud returns "stun:host:port" but GStreamer needs "stun://host:port"
        QString gstStun = stunServer;
        if (gstStun.startsWith("stun:") && !gstStun.startsWith("stun://"))
            gstStun = "stun://" + gstStun.mid(5);
        qDebug() << "SubscribePipeline: STUN server:" << gstStun;
        g_object_set(m_webrtcbin, "stun-server", gstStun.toUtf8().constData(), nullptr);
    }
    g_object_set(m_webrtcbin, "bundle-policy",
                 GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);

    // Disable ICE-TCP gathering -- see PublishPipeline.cpp's ice-agent block
    // for the full rationale (Janus runs ICE-TCP disabled cluster-wide;
    // libnice unconditionally rejects TCP-transport remote candidates either
    // way, so gathering them here is guaranteed-useless dead weight).
    {
        GObject *iceAgent = nullptr;
        g_object_get(m_webrtcbin, "ice-agent", &iceAgent, nullptr);
        if (iceAgent) {
            g_object_set(iceAgent, "ice-tcp", FALSE, nullptr);
            g_object_unref(iceAgent);
        }
    }

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
            // Mask credentials in log output
            QString logUrl = gstUrl;
            logUrl.replace(QRegularExpression("://[^@]+@"), "://***@");
            qDebug() << "SubscribePipeline: adding TURN server" << logUrl;
            gboolean ret = FALSE;
            g_signal_emit_by_name(m_webrtcbin, "add-turn-server", gstUrl.toUtf8().constData(), &ret);
        }
    }

    gst_bin_add(GST_BIN(m_pipeline), m_webrtcbin);

    // No negotiation-needed (we don't create offers)
    g_signal_connect(m_webrtcbin, "on-ice-candidate",
                     G_CALLBACK(onIceCandidate), this);
    g_signal_connect(m_webrtcbin, "pad-added",
                     G_CALLBACK(onPadAdded), this);
    g_signal_connect(m_webrtcbin, "notify::ice-connection-state",
                     G_CALLBACK(onIceStateChanged), this);
    g_signal_connect(m_webrtcbin, "notify::ice-gathering-state",
                     G_CALLBACK(onIceGatheringStateChanged), this);
    g_signal_connect(m_webrtcbin, "on-data-channel",
                     G_CALLBACK(onDataChannel), this);

    // Bus watch — track ID for cleanup
    GstBus *bus = gst_element_get_bus(m_pipeline);
    m_busWatchId = gst_bus_add_watch(bus, [](GstBus *, GstMessage *msg, gpointer) -> gboolean {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError *err = nullptr; gchar *dbg = nullptr;
            gst_message_parse_error(msg, &err, &dbg);
            const gchar *srcName = GST_MESSAGE_SRC(msg)
                ? GST_OBJECT_NAME(GST_MESSAGE_SRC(msg)) : "?";
            qWarning().nospace() << "SubscribePipeline ERROR from " << srcName
                << " [" << (err ? g_quark_to_string(err->domain) : "?")
                << ":" << (err ? err->code : -1) << "] "
                << (err ? err->message : "?") << " | " << (dbg ? dbg : "");
            g_clear_error(&err); g_free(dbg);
        }
        return TRUE;
    }, nullptr);
    gst_object_unref(bus);

    GstStateChangeReturn ret = gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        emit error("Failed to start subscribe pipeline");
        cleanup();
        return false;
    }

    m_running = true;
    qDebug() << "SubscribePipeline: started (receive-only) for" << m_remoteSessionId.left(20);
    return true;
}

void SubscribePipeline::stop()
{
    if (!m_running) return;
    cleanup();
    m_running = false;
    qDebug() << "SubscribePipeline: stopped";
}

void SubscribePipeline::cleanup()
{
    qDebug() << "SubscribePipeline::cleanup() — begin, pipeline=" << (void*)m_pipeline;
    if (m_busWatchId > 0) {
        g_source_remove(m_busWatchId);
        m_busWatchId = 0;
    }
    if (m_pliTimer) { m_pliTimer->stop(); delete m_pliTimer; m_pliTimer = nullptr; }
    if (m_videoAppsink)
        g_signal_handlers_disconnect_by_data(m_videoAppsink, this);
    if (m_webrtcbin)
        g_signal_handlers_disconnect_by_data(m_webrtcbin, this);
    if (m_pipeline) {
        qDebug() << "SubscribePipeline::cleanup() — setting NULL state";
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        qDebug() << "SubscribePipeline::cleanup() — unrefing pipeline";
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
        qDebug() << "SubscribePipeline::cleanup() — pipeline freed";
    }
    m_webrtcbin = nullptr;
    m_videoAppsink = nullptr;
    m_videoConvert = nullptr;
    m_videoDecode = nullptr;
    m_audioChainCreated = false;
    m_remoteDescSet = false;
    m_pendingCandidates.clear();
}

// Janus VideoRoom offers the subscriber stream with RTX (a=ssrc-group:FID
// + an rtx rtpmap) and sometimes red/ulpfec. webrtcbin 1.28 cannot wire up
// those receive transceivers (try_match_transceiver_with_fec_decoder
// fails), leaving an internal transportreceivebin queue unlinked → fatal
// not-linked(-1) + a buffer-storm that freezes the app and times the call
// out. Janus explicitly supports the subscriber shaping the offer SDP, so
// we strip RTX/RED/ULPFEC and negotiate a plain H264 receive (the same
// shape as the audio path, which works). Trade-off: no receive-leg
// retransmission; PLI still drives keyframes and the HPB link is good.
static QString stripReceiveRtxFec(const QString &sdp)
{
    const QStringList lines = sdp.split(QRegularExpression("\r\n|\n"));
    // RTP payload numbers are only unique WITHIN an m= section, so the
    // strip must be scoped to the video section — keying on PT number
    // across the whole SDP would corrupt audio when an audio PT happens
    // to equal a video rtx/red/ulpfec PT.
    QSet<QString> dropPts;        // rtx/red/ulpfec PTs (video section only)
    QSet<QString> rtxSsrcs;       // secondary (rtx) SSRCs from video FID groups
    QRegularExpression rtpmapRx(
        "^a=rtpmap:(\\d+)\\s+(rtx|red|ulpfec)/", QRegularExpression::CaseInsensitiveOption);
    bool inVideo = false;
    for (const QString &ln : lines) {
        if (ln.startsWith("m=")) { inVideo = ln.startsWith("m=video "); continue; }
        if (!inVideo) continue;
        auto m = rtpmapRx.match(ln);
        if (m.hasMatch()) { dropPts.insert(m.captured(1)); continue; }
        if (ln.startsWith("a=ssrc-group:FID")) {
            const QStringList t = ln.section(':', 1).split(' ', Qt::SkipEmptyParts);
            // tokens: "FID", <primary>, <rtx...> — everything past the
            // primary is a retransmission SSRC we will not receive.
            for (int i = 2; i < t.size(); ++i) rtxSsrcs.insert(t[i]);
        }
    }
    if (dropPts.isEmpty() && rtxSsrcs.isEmpty()) return sdp;  // nothing to do

    QStringList out;
    out.reserve(lines.size());
    QRegularExpression ptAttrRx("^a=(rtpmap|fmtp|rtcp-fb):(\\d+)\\b");
    inVideo = false;
    int videoFmtCount = -1;  // formats kept on the rewritten m=video line
    for (const QString &ln : lines) {
        if (ln.startsWith("m=")) {
            inVideo = ln.startsWith("m=video ");
            if (inVideo) {
                // m=video <port> <proto> <fmt>...  — drop rtx/fec PTs;
                // indices 0-2 (m=video, port, proto) always survive.
                const QStringList t = ln.split(' ', Qt::SkipEmptyParts);
                QStringList kept;
                for (int i = 0; i < t.size(); ++i)
                    if (i < 3 || !dropPts.contains(t[i])) kept << t[i];
                videoFmtCount = kept.size() - 3;
                out << kept.join(' ');
                continue;
            }
        }
        if (inVideo) {
            if (ln.startsWith("a=ssrc-group:FID")) continue;
            auto pm = ptAttrRx.match(ln);
            if (pm.hasMatch() && dropPts.contains(pm.captured(2))) continue;
            if (ln.startsWith("a=ssrc:")) {
                const QString id = ln.mid(7).section(' ', 0, 0);
                if (rtxSsrcs.contains(id)) continue;
            }
        }
        out << ln;
    }
    if (videoFmtCount < 1) {
        // Stripping would leave m=video with no codec — never emit that;
        // fall back to the original and let webrtcbin fail loudly instead.
        qWarning() << "SubscribePipeline: RTX/FEC strip would empty m=video "
                      "— keeping original offer";
        return sdp;
    }
    qDebug().nospace() << "SubscribePipeline: stripped RTX/FEC from offer (pts="
                       << QStringList(dropPts.values()).join(',') << " rtxSsrcs="
                       << QStringList(rtxSsrcs.values()).join(',') << ")";
    return out.join("\r\n");
}

void SubscribePipeline::setRemoteOffer(const QString &sdpIn)
{
    if (!m_webrtcbin) return;

    const QString sdp = stripReceiveRtxFec(sdpIn);
    qDebug() << "SubscribePipeline::setRemoteOffer — parsing SDP...";
    QByteArray sdpUtf8 = sdp.toUtf8();
    GstSDPMessage *sdpMsg;
    gst_sdp_message_new(&sdpMsg);
    if (gst_sdp_message_parse_buffer((const guint8 *)sdpUtf8.constData(),
                                     sdpUtf8.size(), sdpMsg) != GST_SDP_OK) {
        gst_sdp_message_free(sdpMsg);
        qWarning() << "SubscribePipeline: failed to parse remote offer SDP";
        emit error("Could not negotiate the incoming video stream "
                   "(bad session description). Try rejoining the call.");
        return;
    }

    qDebug() << "SubscribePipeline::setRemoteOffer — creating session description...";
    GstWebRTCSessionDescription *desc = gst_webrtc_session_description_new(
        GST_WEBRTC_SDP_TYPE_OFFER, sdpMsg);

    qDebug() << "SubscribePipeline::setRemoteOffer — setting remote description on webrtcbin...";
    g_signal_emit_by_name(m_webrtcbin, "set-remote-description", desc, nullptr);
    qDebug() << "SubscribePipeline::setRemoteOffer — remote description set OK";
    gst_webrtc_session_description_free(desc);

    m_remoteDescSet = true;
    qDebug() << "SubscribePipeline: remote offer SDP:\n" << sdp.left(2000);
    qDebug() << "SubscribePipeline: set remote offer, flushing" << m_pendingCandidates.size() << "queued candidates";
    for (const auto &c : m_pendingCandidates)
        g_signal_emit_by_name(m_webrtcbin, "add-ice-candidate", c.first, c.second.toUtf8().constData());
    m_pendingCandidates.clear();

    qDebug() << "SubscribePipeline: creating answer...";
    GstPromise *answerPromise = gst_promise_new_with_change_func(
        onAnswerCreated, this, nullptr);
    g_signal_emit_by_name(m_webrtcbin, "create-answer", nullptr, answerPromise);
}

void SubscribePipeline::addIceCandidate(const QString &candidate, int sdpMLineIndex, const QString &sdpMid)
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

// --- GStreamer callbacks (marshalled to Qt thread) ---

void SubscribePipeline::onIceCandidate(GstElement *, guint mlineIndex, gchar *candidate, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);
    QString c = QString::fromUtf8(candidate);
    int ml = static_cast<int>(mlineIndex);
    QPointer<SubscribePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, c, ml]() {
        if (!guard) return;
        emit guard->iceCandidateReady(c, ml, QString("0"));
    }, Qt::QueuedConnection);
}

void SubscribePipeline::onPadAdded(GstElement *, GstPad *pad, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);

    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, nullptr);
    if (!caps) return;

    GstStructure *s = gst_caps_get_structure(caps, 0);
    const gchar *media = gst_structure_get_string(s, "media");
    const gchar *encoding = gst_structure_get_string(s, "encoding-name");

    {
        gchar *capsStr = gst_caps_to_string(caps);
        qDebug() << "SubscribePipeline: pad added, media=" << media
                 << "encoding=" << encoding << "caps=" << capsStr;
        g_free(capsStr);
    }

    bool isAudio = (media && g_strcmp0(media, "audio") == 0)
                || (encoding && g_ascii_strcasecmp(encoding, "OPUS") == 0);
    bool isVideo = (media && g_strcmp0(media, "video") == 0)
                || (encoding && (g_ascii_strcasecmp(encoding, "VP8") == 0
                              || g_ascii_strcasecmp(encoding, "H264") == 0));

    // Copy encoding string before unreffing caps (encoding points into caps memory)
    QByteArray encodingCopy = encoding ? QByteArray(encoding) : QByteArray();
    gst_caps_unref(caps);

    if (!isAudio && !isVideo) {
        qDebug() << "SubscribePipeline: skipping unknown pad type";
        return;
    }

    // Link pads synchronously on the GStreamer thread — deferring to the Qt
    // thread causes a race where data arrives before the pad is linked,
    // producing "not-linked" errors that kill audio.
    if (isAudio && !self->m_audioChainCreated) {
        self->createAudioChain(pad);
        self->m_audioChainCreated = true;
    } else if (isVideo && !self->m_videoAppsink) {
        self->createVideoChain(pad, encodingCopy.constData());
    }
}

void SubscribePipeline::createAudioChain(GstPad *pad)
{
    GstElement *depay = gst_element_factory_make("rtpopusdepay", nullptr);
    GstElement *dec = gst_element_factory_make("opusdec", nullptr);
    GstElement *convert = gst_element_factory_make("audioconvert", nullptr);
    GstElement *resample = gst_element_factory_make("audioresample", nullptr);

    // Try WASAPI2 sink first (best Windows audio), then wasapisink, then autoaudiosink
    GstElement *sink = nullptr;
    sink = gst_element_factory_make("wasapi2sink", nullptr);
    if (sink) {
        qDebug() << "SubscribePipeline: audio sink: wasapi2sink";
        if (!m_audioOutputDeviceId.isEmpty())
            g_object_set(sink, "device", m_audioOutputDeviceId.toUtf8().constData(), nullptr);
    }
    if (!sink) {
        sink = gst_element_factory_make("wasapisink", nullptr);
        if (sink) {
            qDebug() << "SubscribePipeline: audio sink: wasapisink";
            g_object_set(sink, "low-latency", FALSE, nullptr);
            if (!m_audioOutputDeviceId.isEmpty())
                g_object_set(sink, "device", m_audioOutputDeviceId.toUtf8().constData(), nullptr);
        }
    }
    if (!sink) {
        sink = gst_element_factory_make("directsoundsink", nullptr);
        if (sink) {
            qDebug() << "SubscribePipeline: audio sink: directsoundsink";
            if (!m_audioOutputDeviceId.isEmpty())
                g_object_set(sink, "device", m_audioOutputDeviceId.toUtf8().constData(), nullptr);
        }
    }
    if (!sink) {
        sink = gst_element_factory_make("autoaudiosink", nullptr);
        if (sink) qDebug() << "SubscribePipeline: audio sink: autoaudiosink";
    }

    if (!depay || !dec || !convert || !resample || !sink) {
        qWarning() << "SubscribePipeline: failed to create audio receive chain";
        if (depay) gst_object_unref(depay);
        if (dec) gst_object_unref(dec);
        if (convert) gst_object_unref(convert);
        if (resample) gst_object_unref(resample);
        if (sink) gst_object_unref(sink);
        return;
    }

    // 0.41.0-beta — receive-side loudness chain (mirror PeerPipeline).
    // audiodynamic compressor + volume + rglimiter between opusdec and
    // the sink; matches the tgcalls / libwebrtc AudioProcessing playout.
    QSettings audSettings("TalQ", "TalQ");
    audSettings.beginGroup("Audio");
    const double playbackGain = qBound(0.0,
        audSettings.value("playbackGain", 1.8).toDouble(), 4.0);
    audSettings.endGroup();
    GstElement *dyn   = gst_element_factory_make("audiodynamic", nullptr);
    GstElement *gain  = gst_element_factory_make("volume",       nullptr);
    GstElement *limit = gst_element_factory_make("rglimiter",    nullptr);
    if (dyn) {
        gst_util_set_object_arg(G_OBJECT(dyn), "mode",            "compressor");
        gst_util_set_object_arg(G_OBJECT(dyn), "characteristics", "soft-knee");
        g_object_set(dyn, "threshold", 0.25, "ratio", 0.4, nullptr);
    }
    if (gain) g_object_set(gain, "volume", playbackGain, nullptr);

    const bool haveLoudness = (dyn && gain && limit);
    if (haveLoudness) {
        gst_bin_add_many(GST_BIN(m_pipeline), depay, dec, convert, resample,
                         dyn, gain, limit, sink, nullptr);
        gst_element_link_many(depay, dec, dyn, gain, limit,
                              convert, resample, sink, nullptr);
    } else {
        if (dyn)   gst_object_unref(dyn);
        if (gain)  gst_object_unref(gain);
        if (limit) gst_object_unref(limit);
        gst_bin_add_many(GST_BIN(m_pipeline), depay, dec, convert, resample, sink, nullptr);
        gst_element_link_many(depay, dec, convert, resample, sink, nullptr);
    }
    gst_element_sync_state_with_parent(depay);
    gst_element_sync_state_with_parent(dec);
    if (haveLoudness) {
        gst_element_sync_state_with_parent(dyn);
        gst_element_sync_state_with_parent(gain);
        gst_element_sync_state_with_parent(limit);
    }
    gst_element_sync_state_with_parent(convert);
    gst_element_sync_state_with_parent(resample);
    gst_element_sync_state_with_parent(sink);

    GstPad *sinkPad = gst_element_get_static_pad(depay, "sink");
    GstPadLinkReturn ret = gst_pad_link(pad, sinkPad);
    gst_object_unref(sinkPad);

    if (ret != GST_PAD_LINK_OK)
        qWarning() << "SubscribePipeline: audio receive pad link failed:" << ret;
    else
        qDebug() << "SubscribePipeline: audio receive chain linked successfully";
}

void SubscribePipeline::createVideoChain(GstPad *pad, const gchar *encoding)
{
    qDebug() << "SubscribePipeline: creating video chain for" << encoding;

    // Canonical webrtcbin receive pattern, verbatim from the official
    // gstwebrtc-demos webrtc-recvonly-h264.c: link the webrtcbin src pad
    // straight into decodebin. decodebin autoplugs the RTP depayloader +
    // parser + best-ranked (hardware) decoder and negotiates caps, and is
    // the path that handles H264 + RTX + in-band SPS/PPS uniformly. The
    // two hand-built chains (rtph264depay→decoder, then
    // rtph264depay→h264parse→decodebin) both hit not-negotiated(-4); the
    // working audio path is manual too, so the defect is the manual H264
    // depay specifically — decodebin from the RTP pad is the documented fix.
    const bool isVP8 = encoding && g_ascii_strcasecmp(encoding, "VP8") == 0;
    m_videoCodec   = isVP8 ? QStringLiteral("VP8") : QStringLiteral("H264");
    m_videoDecoder = QStringLiteral("auto");  // decodebin selects at runtime

    GstElement *decode  = gst_element_factory_make("decodebin", nullptr);
    GstElement *convert = gst_element_factory_make("videoconvert", nullptr);
    GstElement *appsink = gst_element_factory_make("appsink", nullptr);

    if (!decode || !convert || !appsink) {
        qWarning() << "SubscribePipeline: failed to create video elements";
        for (GstElement *e : { decode, convert, appsink })
            if (e) gst_object_unref(e);
        return;
    }

    GstCaps *sinkCaps = gst_caps_from_string("video/x-raw,format=BGRx");
    g_object_set(appsink,
        "emit-signals", TRUE,
        "caps", sinkCaps,
        "drop", TRUE,
        "max-buffers", 1,
        nullptr);
    gst_caps_unref(sinkCaps);

    g_signal_connect(appsink, "new-sample",
        G_CALLBACK(onNewVideoSample), this);
    m_videoAppsink = appsink;
    m_videoConvert = convert;
    m_videoDecode  = decode;  // pipeline-owned; PLI sent on its sink pad

    gst_bin_add_many(GST_BIN(m_pipeline), decode, convert, appsink, nullptr);

    // decodebin exposes its decoded video pad asynchronously → onDecodebinPad
    g_signal_connect(decode, "pad-added", G_CALLBACK(onDecodebinPad), this);
    gst_element_link(convert, appsink);

    gst_element_sync_state_with_parent(decode);
    gst_element_sync_state_with_parent(convert);
    gst_element_sync_state_with_parent(appsink);

    GstPad *dbSink = gst_element_get_static_pad(decode, "sink");
    GstPadLinkReturn ret = gst_pad_link(pad, dbSink);
    gst_object_unref(dbSink);  // not retained; PLI re-fetches it per call

    if (ret != GST_PAD_LINK_OK) {
        qWarning() << "SubscribePipeline: webrtcbin pad -> decodebin link failed:" << ret;
        return;
    }
    qDebug() << "SubscribePipeline: webrtcbin video pad -> decodebin linked";

    // Keyframe requests for SFU late-join: GstForceKeyUnit pushed UPSTREAM
    // on decodebin's sink pad (peer of the webrtcbin src pad) → webrtcbin
    // emits RTCP PLI. The pad is fetched fresh each call (no raw pad kept
    // across the GStreamer/Qt thread boundary); m_videoDecode is a
    // pipeline-owned element pointer like m_videoConvert.
    auto sendPLI = [this]() {
        if (!m_pipeline) return;
        // 0.52.9 — the keyframe request (GstForceKeyUnit, an UPSTREAM event) MUST
        // travel sink→src to reach webrtcbin's rtpsession, which is what actually
        // emits the RTCP PLI on the wire. The OLD code sent it via
        // gst_pad_send_event() on decodebin's SINK pad — but gstpad.c gates events
        // by direction: a SINK pad + UPSTREAM event hits the "wrong_direction"
        // path → g_warning("…sending custom-upstream event in wrong direction") +
        // gst_event_unref + return FALSE. The event was DESTROYED at the pad
        // boundary, so NO PLI ever reached the wire: the 5s PLI loop was a pure
        // no-op and the receiver froze on a stale frame, "recovering" only when a
        // periodic GOP IDR (~every 4s, gop=120) happened to roll around — the
        // ~20s-to-one-frame freeze seen on rapid screen-share switches.
        // gst_element_send_event() on the PIPELINE routes the upstream event to
        // the sink elements and walks it up to webrtcbin — exactly what the camera
        // subscriber (SubscribeWebrtcSrc) and PublishPipeline already do.
        const gboolean ok = gst_element_send_event(m_pipeline,
            gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM,
                gst_structure_new("GstForceKeyUnit",
                    "all-headers", G_TYPE_BOOLEAN, TRUE, nullptr)));
        qInfo().nospace() << "SubscribePipeline: screen PLI -> "
                          << (ok ? "ACCEPTED (RTCP keyframe requested)"
                                 : "REJECTED (no element handled it)");
    };
    sendPLI();
    QPointer<SubscribePipeline> guard(this);
    QMetaObject::invokeMethod(this, [guard, sendPLI]() {
        if (!guard) return;
        auto *self = guard.data();
        QTimer::singleShot(500, self, sendPLI);
        QTimer::singleShot(1500, self, sendPLI);
        QTimer::singleShot(3000, self, sendPLI);
        self->m_pliTimer = new QTimer(self);
        self->m_pliTimer->setInterval(5000);
        QObject::connect(self->m_pliTimer, &QTimer::timeout, self, sendPLI);
        self->m_pliTimer->start();
    }, Qt::QueuedConnection);
}

void SubscribePipeline::requestKeyframe()
{
    // 0.52.15 — on-demand RTCP PLI. The CallManager screen-sub startup-grace branch
    // calls this to hurry the first keyframe on a re-offer WITHOUT a rebuild (the
    // rebuild-thrash that left a re-shared screen stuck on "Starting"). Same
    // mechanism as the build-time sendPLI cascade above: a GstForceKeyUnit UPSTREAM
    // event via gst_element_send_event() on the PIPELINE (NOT gst_pad_send_event on
    // a sink pad — that hits the wrong-direction gate and is destroyed; see sendPLI).
    if (!m_pipeline) return;
    const gboolean ok = gst_element_send_event(m_pipeline,
        gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM,
            gst_structure_new("GstForceKeyUnit",
                "all-headers", G_TYPE_BOOLEAN, TRUE, nullptr)));
    qInfo().nospace() << "SubscribePipeline: screen PLI (on-demand) -> "
                      << (ok ? "ACCEPTED (RTCP keyframe requested)"
                             : "REJECTED (no element handled it)");
}

void SubscribePipeline::onDecodebinPad(GstElement *, GstPad *pad, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);
    if (!self->m_videoConvert) return;
    // decodebin only ever produces the one decoded video stream here
    // (it is fed a single video-only RTP branch). Link its raw pad to
    // videoconvert; ignore a second call if already linked.
    GstPad *sink = gst_element_get_static_pad(self->m_videoConvert, "sink");
    if (!sink) return;
    if (!gst_pad_is_linked(sink)) {
        GstPadLinkReturn r = gst_pad_link(pad, sink);
        if (r == GST_PAD_LINK_OK)
            qDebug() << "SubscribePipeline: decodebin -> videoconvert linked";
        else
            qWarning() << "SubscribePipeline: decodebin -> convert link failed:" << r;
    }
    gst_object_unref(sink);
}

GstFlowReturn SubscribePipeline::onNewVideoSample(GstAppSink *sink, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_OK;

    QPointer<SubscribePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, sample]() {
        if (guard && guard->m_videoProvider)
            guard->m_videoProvider->feedFrame(sample);
        gst_sample_unref(sample);
    }, Qt::QueuedConnection);

    return GST_FLOW_OK;
}

void SubscribePipeline::onAnswerCreated(GstPromise *promise, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);

    const GstStructure *reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription *answer = nullptr;
    gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);

    if (!answer) {
        qWarning() << "SubscribePipeline: failed to create answer";
        gst_promise_unref(promise);
        return;
    }

    if (!self->m_webrtcbin) {
        gst_webrtc_session_description_free(answer);
        gst_promise_unref(promise);
        return;
    }
    g_signal_emit_by_name(self->m_webrtcbin, "set-local-description", answer, nullptr);

    gchar *sdpText = gst_sdp_message_as_text(answer->sdp);
    QString sdp = QString::fromUtf8(sdpText);
    g_free(sdpText);
    gst_webrtc_session_description_free(answer);
    gst_promise_unref(promise);

    qDebug() << "SubscribePipeline: answer SDP:\n" << sdp.left(2000);
    qDebug() << "SubscribePipeline: answer created, SDP length=" << sdp.length();
    QPointer<SubscribePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, sdp]() {
        if (!guard) return;
        emit guard->localAnswerReady(sdp);
    }, Qt::QueuedConnection);
}

void SubscribePipeline::onIceStateChanged(GObject *obj, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);
    GstWebRTCICEConnectionState state;
    g_object_get(obj, "ice-connection-state", &state, nullptr);
    const char *names[] = {"new", "checking", "connected", "completed", "failed", "disconnected", "closed"};
    int idx = static_cast<int>(state);
    QString stateName = (idx < 7) ? names[idx] : "unknown";
    QPointer<SubscribePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard, stateName]() {
        if (!guard) return;
        qDebug() << "SubscribePipeline: ICE ->" << stateName;
        emit guard->iceStateChanged(stateName);
    }, Qt::QueuedConnection);
}

void SubscribePipeline::onIceGatheringStateChanged(GObject *obj, GParamSpec *, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);
    GstWebRTCICEGatheringState state;
    g_object_get(obj, "ice-gathering-state", &state, nullptr);
    if (state != GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) return;
    QPointer<SubscribePipeline> guard(self);
    QMetaObject::invokeMethod(self, [guard]() {
        if (!guard) return;
        qDebug() << "SubscribePipeline: ICE gathering complete";
        emit guard->iceGatheringComplete();
    }, Qt::QueuedConnection);
}

void SubscribePipeline::onDataChannel(GstElement *, GstWebRTCDataChannel *channel, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);
    qDebug() << "SubscribePipeline: incoming data channel";
    g_signal_connect(channel, "on-message-string",
                     G_CALLBACK(onDataChannelMessage), self);
}

void SubscribePipeline::onDataChannelMessage(GstWebRTCDataChannel *, gchar *str, gpointer userData)
{
    auto *self = static_cast<SubscribePipeline *>(userData);
    QByteArray raw(str);
    QJsonDocument doc = QJsonDocument::fromJson(raw);
    if (doc.isNull()) return;

    QJsonObject obj = doc.object();
    QString type = obj.value("type").toString();
    if (type.isEmpty()) return;

    QPointer<SubscribePipeline> guard(self);
    if (type == "talq.client") {
        const QString client = obj.value("client").toString();
        const QString version = obj.value("version").toString();
        QMetaObject::invokeMethod(self, [guard, client, version]() {
            if (!guard) return;
            qDebug() << "SubscribePipeline: peer client" << client << version;
            emit guard->peerClientInfo(client, version);
        }, Qt::QueuedConnection);
        return;
    }

    QMetaObject::invokeMethod(self, [guard, type]() {
        if (!guard) return;
        qDebug() << "SubscribePipeline: DC message:" << type;
        emit guard->mediaStateReceived(type);
    }, Qt::QueuedConnection);
}

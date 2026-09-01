#include "core/VoiceRecorder.h"

#include <QDebug>
#include <QFile>

namespace {
constexpr int kTickMs = 100;

// 64 kbit mono is the speech sweet spot: indistinguishable from higher rates
// on a voice note, and small enough that a one-minute message is ~470 KB.
constexpr int kBitrateKbps = 64;

// Loudest peak, in dBFS, below which a whole recording counts as CAPTURING
// NOTHING. This is a test of the microphone, not of the content: a quiet
// message is still a message, and refusing to send real audio would be a
// worse failure than sending a soft one.
//
// Measured on this hardware rather than guessed:
//   dead / muted capture ....... -95 to -79 dBFS
//   live mic, silent room ...... -69 to -57 dBFS
//   ordinary speech ............ -46 to -31 dBFS
// -75 sits between the first two bands, so it separates "the device produced
// digital silence" from "the device works and the room was quiet", and can
// never reach up into speech. An earlier -50 would have refused a genuinely
// quiet recording, which is the one outcome not worth risking.
constexpr double kSilenceDb = -75.0;
}

VoiceRecorder::VoiceRecorder(QObject *parent) : QObject(parent)
{
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, [this]() {
        if (!m_pipeline)
            return;

        // Drain errors here: a Qt event loop never dispatches a GLib bus
        // watch, so this is the only place a mid-recording failure surfaces.
        // Without it a dead microphone would record silence for as long as the
        // user cared to talk.
        if (GstBus *bus = gst_element_get_bus(m_pipeline)) {
            // Watch the loudest thing the microphone actually produced. A
            // recording that never rises off the noise floor is not a message,
            // it is a muted mic -- and the app has to say so rather than send
            // somebody a file with nothing in it.
            while (GstMessage *lm = gst_bus_pop_filtered(bus, GST_MESSAGE_ELEMENT)) {
                const GstStructure *st = gst_message_get_structure(lm);
                if (st && gst_structure_has_name(st, "level")) {
                    // `level` posts peak as a GValueArray, NOT a GstValueList.
                    // Reading it with the gst_value_list_* accessors finds
                    // nothing and silently reports every recording as silent --
                    // which would have made this check worse than useless, since
                    // it would have refused to send anything at all. Both forms
                    // are handled so a future GStreamer that switches cannot
                    // quietly break it again.
                    const GValue *arr = gst_structure_get_value(st, "peak");
                    if (arr && GST_VALUE_HOLDS_LIST(arr)) {
                        const guint n = gst_value_list_get_size(arr);
                        for (guint i = 0; i < n; ++i) {
                            const gdouble db = g_value_get_double(
                                gst_value_list_get_value(arr, i));
                            if (db > m_peakDb) m_peakDb = db;
                        }
                    } else if (arr && G_VALUE_HOLDS(arr, G_TYPE_VALUE_ARRAY)) {
                        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
                        auto *va = static_cast<GValueArray *>(g_value_get_boxed(arr));
                        for (guint i = 0; va && i < va->n_values; ++i) {
                            const gdouble db =
                                g_value_get_double(g_value_array_get_nth(va, i));
                            if (db > m_peakDb) m_peakDb = db;
                        }
                        G_GNUC_END_IGNORE_DEPRECATIONS
                    }
                }
                gst_message_unref(lm);
            }
            if (GstMessage *msg = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
                GError *err = nullptr;
                gchar *dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                const QString reason = err ? QString::fromUtf8(err->message)
                                           : tr("Recording error");
                qWarning() << "VoiceRecorder error:" << reason << (dbg ? dbg : "");
                if (err) g_error_free(err);
                g_free(dbg);
                gst_message_unref(msg);
                gst_object_unref(bus);
                teardown(/*deleteFile*/ true);
                emit failed(reason);
                emit stateChanged();
                return;
            }
            gst_object_unref(bus);
        }

        gint64 pos = 0;
        if (gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &pos))
            m_elapsedMs = pos / GST_MSECOND;
        else
            m_elapsedMs += kTickMs;   // before the clock runs, count ticks
        emit tick();
    });
}

VoiceRecorder::~VoiceRecorder()
{
    teardown(/*deleteFile*/ false);
}

void VoiceRecorder::start(const QString &outPath)
{
    if (m_recording)
        return;

    // wasapi2src is the same capture element the call path uses. It is NAMED so
    // the device can be set below: an unnamed, unset wasapi2src opens whatever
    // WASAPI calls the default endpoint rather than the microphone the user
    // actually chose in Settings, and on a machine where those differ it
    // records the wrong input. PublishPipeline, PeerPipeline and MicTester all
    // set the device; this was the one capture site that did not.
    GError *err = nullptr;
    const QString desc = QStringLiteral(
        "wasapi2src name=micsrc ! audioconvert ! audioresample "
        "! audio/x-raw,rate=48000,channels=1 "
        "! level name=lvl post-messages=true interval=100000000 "
        "! lamemp3enc target=bitrate bitrate=%1 cbr=true "
        "! filesink location=\"%2\"")
        .arg(kBitrateKbps).arg(QString(outPath).replace(QLatin1Char('\\'), QLatin1Char('/')));

    m_pipeline = gst_parse_launch(desc.toUtf8().constData(), &err);
    if (!m_pipeline) {
        const QString reason = err ? QString::fromUtf8(err->message)
                                   : tr("Could not build the recording pipeline.");
        if (err) g_error_free(err);
        emit failed(reason);
        return;
    }
    if (err) g_error_free(err);

    // Point it at the SELECTED microphone, exactly as PublishPipeline,
    // PeerPipeline and MicTester all already do. MediaDeviceManager hands out
    // the {GUID} `device.id` form, because wasapi2src silently fails to open a
    // friendly NAME -- a lesson already recorded in that file after it made the
    // mic test stay flat and calls fall back to the default. Empty means the
    // user has chosen nothing, so the system default is the right answer.
    if (!m_deviceId.isEmpty()) {
        if (GstElement *src = gst_bin_get_by_name(GST_BIN(m_pipeline), "micsrc")) {
            g_object_set(src, "device", m_deviceId.toUtf8().constData(), nullptr);
            gst_object_unref(src);
        }
    }

    m_path = outPath;
    m_elapsedMs = 0;
    m_peakDb = -1000.0;

    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING)
        == GST_STATE_CHANGE_FAILURE) {
        teardown(/*deleteFile*/ true);
        emit failed(tr("Could not open the microphone."));
        return;
    }

    m_recording = true;
    m_timer.start();
    emit stateChanged();
}

void VoiceRecorder::stop()
{
    if (!m_pipeline || !m_recording)
        return;

    m_timer.stop();
    m_recording = false;

    // EOS, then WAIT for it to reach the sink. Setting the pipeline straight to
    // NULL truncates the file: lamemp3enc still holds buffered frames and
    // filesink has not flushed, so the mp3 ends mid-frame. The wait is bounded
    // because a hung element must not freeze the UI thread -- on timeout the
    // file is still written, just possibly a few frames short, which is a far
    // better failure than a hang.
    gst_element_send_event(m_pipeline, gst_event_new_eos());
    if (GstBus *bus = gst_element_get_bus(m_pipeline)) {
        GstMessage *msg = gst_bus_timed_pop_filtered(
            bus, 3 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (msg) gst_message_unref(msg);
        gst_object_unref(bus);
    }

    const QString path = m_path;
    const qint64 ms = m_elapsedMs;
    teardown(/*deleteFile*/ false);
    emit stateChanged();

    // A recording with no bytes is a failed capture wearing a filename.
    if (QFile(path).size() <= 0) {
        QFile::remove(path);
        emit failed(tr("Nothing was recorded."));
        return;
    }
    // And a recording that is all noise floor is the same failure with bytes
    // in it. An mp3 of silence is the same SIZE as an mp3 of speech, so the
    // file existing proves nothing -- which is exactly how a muted microphone
    // shipped two voice messages that turned out to be empty. Say it here,
    // where it can still be acted on, rather than letting someone send it.
    if (m_peakDb < kSilenceDb) {
        QFile::remove(path);
        emit failed(tr("No sound was recorded — your microphone appears to be "
                       "muted. Check the mute key or switch on your device's "
                       "microphone, then try again."));
        return;
    }
    emit finished(path, ms);
}

void VoiceRecorder::cancel()
{
    if (!m_pipeline)
        return;
    m_timer.stop();
    m_recording = false;
    teardown(/*deleteFile*/ true);
    emit stateChanged();
}

void VoiceRecorder::teardown(bool deleteFile)
{
    m_timer.stop();
    if (m_pipeline) {
        gst_element_set_state(m_pipeline, GST_STATE_NULL);
        gst_object_unref(m_pipeline);
        m_pipeline = nullptr;
    }
    m_recording = false;
    if (deleteFile && !m_path.isEmpty())
        QFile::remove(m_path);
    m_path.clear();
    m_elapsedMs = 0;
}

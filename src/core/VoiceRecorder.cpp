#include "core/VoiceRecorder.h"

#include <QDebug>
#include <QFile>

namespace {
constexpr int kTickMs = 100;

// 64 kbit mono is the speech sweet spot: indistinguishable from higher rates
// on a voice note, and small enough that a one-minute message is ~470 KB.
constexpr int kBitrateKbps = 64;
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

    // wasapi2src is the same capture element the call path uses, so a machine
    // whose microphone works in a TalQ call records here too -- no second
    // device story to get wrong.
    GError *err = nullptr;
    const QString desc = QStringLiteral(
        "wasapi2src ! audioconvert ! audioresample "
        "! audio/x-raw,rate=48000,channels=1 "
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

    m_path = outPath;
    m_elapsedMs = 0;

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

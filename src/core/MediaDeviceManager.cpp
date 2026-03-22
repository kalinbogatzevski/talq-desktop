#include "core/MediaDeviceManager.h"
#include <QDebug>

MediaDeviceManager::MediaDeviceManager(QObject *parent)
    : QObject(parent)
{
}

MediaDeviceManager::~MediaDeviceManager() = default;

void MediaDeviceManager::refresh()
{
    m_audioInputs.clear();
    m_audioOutputs.clear();
    m_videoInputs.clear();

    GstDeviceMonitor *monitor = gst_device_monitor_new();

    gst_device_monitor_add_filter(monitor, "Audio/Source", nullptr);
    gst_device_monitor_add_filter(monitor, "Audio/Sink", nullptr);
    gst_device_monitor_add_filter(monitor, "Video/Source", nullptr);

    if (!gst_device_monitor_start(monitor)) {
        qWarning() << "MediaDeviceManager: failed to start device monitor";
        gst_object_unref(monitor);
        return;
    }

    GList *devices = gst_device_monitor_get_devices(monitor);
    for (GList *it = devices; it; it = it->next) {
        GstDevice *dev = GST_DEVICE(it->data);
        gchar *name = gst_device_get_display_name(dev);
        gchar *cls = gst_device_get_device_class(dev);

        MediaDevice md;
        md.name = QString::fromUtf8(name);
        GstStructure *props = gst_device_get_properties(dev);
        if (props) {
            const gchar *strid = gst_structure_get_string(props, "device.strid");
            if (!strid)
                strid = gst_structure_get_string(props, "device.path");
            md.id = strid ? QString::fromUtf8(strid) : md.name;
            gst_structure_free(props);
        } else {
            md.id = md.name;
        }

        QString deviceClass = QString::fromUtf8(cls);
        if (deviceClass.contains("Source") && deviceClass.contains("Audio")) {
            md.type = "audio-input";
            m_audioInputs.append(md);
        } else if (deviceClass.contains("Sink") && deviceClass.contains("Audio")) {
            md.type = "audio-output";
            m_audioOutputs.append(md);
        } else if (deviceClass.contains("Source") && deviceClass.contains("Video")) {
            md.type = "video-input";
            m_videoInputs.append(md);
        }

        g_free(name);
        g_free(cls);
        gst_object_unref(dev);
    }
    g_list_free(devices);

    gst_device_monitor_stop(monitor);
    gst_object_unref(monitor);

    qDebug() << "MediaDeviceManager: found"
             << m_audioInputs.size() << "mic(s),"
             << m_audioOutputs.size() << "speaker(s),"
             << m_videoInputs.size() << "camera(s)";

    emit devicesChanged();
    restoreDevices();
}

void MediaDeviceManager::setSelectedAudioInput(int idx)
{
    if (m_selectedInput != idx) {
        m_selectedInput = idx;
        emit selectedChanged();
        if (!m_restoring) saveDevices();
    }
}

void MediaDeviceManager::setSelectedAudioOutput(int idx)
{
    if (m_selectedOutput != idx) {
        m_selectedOutput = idx;
        emit selectedChanged();
        if (!m_restoring) saveDevices();
    }
}

void MediaDeviceManager::setSelectedVideoInput(int idx)
{
    if (m_selectedVideo != idx) {
        m_selectedVideo = idx;
        emit selectedChanged();
        if (!m_restoring) saveDevices();
    }
}

QString MediaDeviceManager::selectedInputName() const
{
    if (m_selectedInput >= 0 && m_selectedInput < m_audioInputs.size())
        return m_audioInputs[m_selectedInput].name;
    return {};
}

QString MediaDeviceManager::selectedOutputName() const
{
    if (m_selectedOutput >= 0 && m_selectedOutput < m_audioOutputs.size())
        return m_audioOutputs[m_selectedOutput].name;
    return {};
}

static QStringList deviceNames(const QVector<MediaDevice> &devices)
{
    QStringList names;
    names.reserve(devices.size());
    for (const auto &d : devices)
        names << d.name;
    return names;
}

QStringList MediaDeviceManager::audioInputNames() const  { return deviceNames(m_audioInputs); }
QStringList MediaDeviceManager::audioOutputNames() const { return deviceNames(m_audioOutputs); }
QStringList MediaDeviceManager::videoInputNames() const  { return deviceNames(m_videoInputs); }

QString MediaDeviceManager::selectedInputDeviceId() const
{
    if (m_selectedInput >= 0 && m_selectedInput < m_audioInputs.size())
        return m_audioInputs[m_selectedInput].id;
    return {};
}

QString MediaDeviceManager::selectedOutputDeviceId() const
{
    if (m_selectedOutput >= 0 && m_selectedOutput < m_audioOutputs.size())
        return m_audioOutputs[m_selectedOutput].id;
    return {};
}

void MediaDeviceManager::saveDevices()
{
    m_settings.beginGroup("Devices");
    if (m_selectedInput >= 0 && m_selectedInput < m_audioInputs.size()) {
        m_settings.setValue("audioInputName", m_audioInputs[m_selectedInput].name);
        m_settings.setValue("audioInputId", m_audioInputs[m_selectedInput].id);
    }
    if (m_selectedOutput >= 0 && m_selectedOutput < m_audioOutputs.size()) {
        m_settings.setValue("audioOutputName", m_audioOutputs[m_selectedOutput].name);
        m_settings.setValue("audioOutputId", m_audioOutputs[m_selectedOutput].id);
    }
    if (m_selectedVideo >= 0 && m_selectedVideo < m_videoInputs.size()) {
        m_settings.setValue("videoInputName", m_videoInputs[m_selectedVideo].name);
        m_settings.setValue("videoInputId", m_videoInputs[m_selectedVideo].id);
    }
    m_settings.endGroup();
}

void MediaDeviceManager::restoreDevices()
{
    m_restoring = true;
    m_settings.beginGroup("Devices");
    auto matchDevice = [](const QVector<MediaDevice> &list, const QString &name, const QString &id) -> int {
        QVector<int> nameMatches;
        for (int i = 0; i < list.size(); ++i) {
            if (list[i].name == name)
                nameMatches.append(i);
        }
        if (nameMatches.size() == 1)
            return nameMatches.first();
        if (nameMatches.size() > 1) {
            for (int idx : nameMatches) {
                if (list[idx].id == id)
                    return idx;
            }
            return nameMatches.first();
        }
        return -1;
    };

    QString inName = m_settings.value("audioInputName").toString();
    QString inId = m_settings.value("audioInputId").toString();
    if (!inName.isEmpty()) {
        int idx = matchDevice(m_audioInputs, inName, inId);
        if (idx >= 0) setSelectedAudioInput(idx);
    }

    QString outName = m_settings.value("audioOutputName").toString();
    QString outId = m_settings.value("audioOutputId").toString();
    if (!outName.isEmpty()) {
        int idx = matchDevice(m_audioOutputs, outName, outId);
        if (idx >= 0) setSelectedAudioOutput(idx);
    }

    QString vidName = m_settings.value("videoInputName").toString();
    QString vidId = m_settings.value("videoInputId").toString();
    if (!vidName.isEmpty()) {
        int idx = matchDevice(m_videoInputs, vidName, vidId);
        if (idx >= 0) setSelectedVideoInput(idx);
    }

    m_settings.endGroup();
    m_restoring = false;
    qDebug() << "MediaDeviceManager: restored devices — mic:" << m_selectedInput
             << "speaker:" << m_selectedOutput << "camera:" << m_selectedVideo;
}

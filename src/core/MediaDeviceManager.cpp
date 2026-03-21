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
}

QStringList MediaDeviceManager::audioInputNames() const
{
    QStringList names;
    for (const auto &d : m_audioInputs)
        names << d.name;
    return names;
}

QStringList MediaDeviceManager::audioOutputNames() const
{
    QStringList names;
    for (const auto &d : m_audioOutputs)
        names << d.name;
    return names;
}

QStringList MediaDeviceManager::videoInputNames() const
{
    QStringList names;
    for (const auto &d : m_videoInputs)
        names << d.name;
    return names;
}

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

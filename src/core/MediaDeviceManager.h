#pragma once

#include <QObject>
#include <QStringList>
#include <QVector>
#include <gst/gst.h>

struct MediaDevice {
    QString id;
    QString name;
    QString type;  // "audio-input", "audio-output", "video-input"
};

class MediaDeviceManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList audioInputNames READ audioInputNames NOTIFY devicesChanged)
    Q_PROPERTY(QStringList audioOutputNames READ audioOutputNames NOTIFY devicesChanged)
    Q_PROPERTY(QStringList videoInputNames READ videoInputNames NOTIFY devicesChanged)

public:
    explicit MediaDeviceManager(QObject *parent = nullptr);
    ~MediaDeviceManager() override;

    Q_INVOKABLE void refresh();

    Q_PROPERTY(int selectedAudioInput READ selectedAudioInput WRITE setSelectedAudioInput NOTIFY selectedChanged)
    Q_PROPERTY(int selectedAudioOutput READ selectedAudioOutput WRITE setSelectedAudioOutput NOTIFY selectedChanged)

    int selectedAudioInput() const { return m_selectedInput; }
    int selectedAudioOutput() const { return m_selectedOutput; }
    void setSelectedAudioInput(int idx) { if (m_selectedInput != idx) { m_selectedInput = idx; emit selectedChanged(); } }
    void setSelectedAudioOutput(int idx) { if (m_selectedOutput != idx) { m_selectedOutput = idx; emit selectedChanged(); } }

    // Get selected device name for pipeline configuration
    QString selectedInputName() const { return (m_selectedInput >= 0 && m_selectedInput < m_audioInputs.size()) ? m_audioInputs[m_selectedInput].name : QString(); }
    QString selectedOutputName() const { return (m_selectedOutput >= 0 && m_selectedOutput < m_audioOutputs.size()) ? m_audioOutputs[m_selectedOutput].name : QString(); }

    QStringList audioInputNames() const;
    QStringList audioOutputNames() const;
    QStringList videoInputNames() const;

    const QVector<MediaDevice> &audioInputs() const { return m_audioInputs; }
    const QVector<MediaDevice> &audioOutputs() const { return m_audioOutputs; }
    const QVector<MediaDevice> &videoInputs() const { return m_videoInputs; }

signals:
    void devicesChanged();
    void selectedChanged();

private:
    int m_selectedInput = -1;   // -1 = system default
    int m_selectedOutput = -1;
    QVector<MediaDevice> m_audioInputs;
    QVector<MediaDevice> m_audioOutputs;
    QVector<MediaDevice> m_videoInputs;
};

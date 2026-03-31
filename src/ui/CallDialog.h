#pragma once

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QImage>
#include <QPixmap>
#include "core/CallManager.h"
#include "core/ApiClient.h"

class VideoWidget : public QWidget
{
    Q_OBJECT
public:
    explicit VideoWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMinimumSize(160, 120);
    }
    void setImage(const QImage &img) { m_image = img; update(); }
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QImage m_image;
};

class CallDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CallDialog(CallManager *callManager, ApiClient *api, QWidget *parent = nullptr);

private slots:
    void onStateChanged();
    void onDurationChanged();
    void onMuteChanged();
    void onCameraChanged();
    void onRemoteFrame(const QImage &image);

private:
    void buildUi();
    void showIncomingMode();
    void showActiveMode();
    void connectVideoProviders();
    QString formatDuration(int seconds) const;

    CallManager *m_callManager;
    ApiClient *m_api;
    void fetchAvatar(const QString &userId);

    // UI elements
    QLabel *m_avatarLabel = nullptr;
    QLabel *m_peerLabel = nullptr;
    QPixmap m_avatarPixmap;
    QString m_loadedAvatarUserId;
    QLabel *m_stateLabel = nullptr;
    QLabel *m_statusDetailLabel = nullptr;
    QLabel *m_durationLabel = nullptr;
    VideoWidget *m_remoteVideo = nullptr;
    VideoWidget *m_localPreview = nullptr;

    // Active call buttons
    QPushButton *m_muteBtn = nullptr;
    QPushButton *m_hangupBtn = nullptr;
    QPushButton *m_cameraBtn = nullptr;

    // Incoming call buttons
    QPushButton *m_acceptBtn = nullptr;
    QPushButton *m_declineBtn = nullptr;

    QWidget *m_activeRow = nullptr;
    QWidget *m_incomingRow = nullptr;
    bool m_videoConnected = false;
    bool m_localConnected = false;
    QObject *m_lastRemoteProvider = nullptr;
    QObject *m_lastLocalProvider = nullptr;
};

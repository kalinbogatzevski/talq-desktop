#pragma once

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include "core/CallManager.h"

class CallDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CallDialog(CallManager *callManager, QWidget *parent = nullptr);

private slots:
    void onStateChanged();
    void onDurationChanged();
    void onMuteChanged();
    void onCameraChanged();

private:
    void buildUi();
    void showIncomingMode();
    void showActiveMode();
    QString formatDuration(int seconds) const;

    CallManager *m_callManager;

    // UI elements
    QLabel *m_peerLabel = nullptr;
    QLabel *m_stateLabel = nullptr;
    QLabel *m_statusDetailLabel = nullptr;
    QLabel *m_durationLabel = nullptr;

    // Active call buttons
    QPushButton *m_muteBtn = nullptr;
    QPushButton *m_hangupBtn = nullptr;
    QPushButton *m_cameraBtn = nullptr;

    // Incoming call buttons
    QPushButton *m_acceptBtn = nullptr;
    QPushButton *m_declineBtn = nullptr;

    QWidget *m_activeRow = nullptr;
    QWidget *m_incomingRow = nullptr;
};

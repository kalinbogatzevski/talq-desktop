#pragma once

#include <QDialog>

class ApiClient;
class QLabel;
class QSpinBox;
class QPushButton;
class QLineEdit;
class QComboBox;

/**
 * Breakout rooms — moderator controls for splitting a conversation up.
 *
 * Moderator-only, and only where the server advertises `breakout-rooms-v1`.
 * The flow the server models is three separate steps, and conflating them is
 * the usual way to get this wrong:
 *
 *   configure  create N rooms and decide how people are assigned
 *   start      open them — this is what actually MOVES participants
 *   stop       close them and bring everyone back
 *   remove     delete the rooms entirely
 *
 * ⚠ Participants do not move because this client asks them to. The server
 * tells each client, over signaling, which conversation it is now in
 * (SignalingClient::switchedToRoom). That is why TalQ has to handle being
 * moved as well as doing the moving — a moderator running this against
 * clients that ignore the signal would split the room in the server's opinion
 * only.
 */
class BreakoutRoomsDialog : public QDialog
{
    Q_OBJECT

public:
    BreakoutRoomsDialog(ApiClient *api, const QString &token, QWidget *parent = nullptr);

private:
    void configure();
    void start();
    void stop();
    void remove();
    void broadcast();
    void say(const QString &text, bool bad = false);

    ApiClient  *m_api;
    QString     m_token;

    QComboBox   *m_mode      = nullptr;
    QSpinBox    *m_amount    = nullptr;
    QLineEdit   *m_broadcast = nullptr;
    QPushButton *m_createBtn = nullptr;
    QPushButton *m_startBtn  = nullptr;
    QPushButton *m_stopBtn   = nullptr;
    QPushButton *m_removeBtn = nullptr;
    QLabel      *m_status    = nullptr;
};

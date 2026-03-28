#pragma once

#include <QWidget>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QVBoxLayout>

class SignalingClient;
class MessageListModel;

class ComposerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ComposerWidget(QWidget *parent = nullptr);

    void setTopicName(const QString &name);
    void setSignaling(SignalingClient *sig);
    void setMessageModel(MessageListModel *model);
    void setInputFont(const QFont &font);
    void handlePastedImage(const QString &path);
    void handlePastedFile(const QString &path);

signals:
    void sendMessage(const QString &text);

private slots:
    void sendAction();
    void showPendingFile(const QString &path);
    void confirmSendFile();
    void cancelPendingFile();

private:
    QTextEdit *m_input = nullptr;
    QPushButton *m_sendBtn = nullptr;
    QPushButton *m_attachBtn = nullptr;
    SignalingClient *m_signaling = nullptr;
    MessageListModel *m_model = nullptr;
    QString m_topicName;

    // Pending file confirmation bar
    QWidget *m_pendingBar = nullptr;
    QLabel *m_pendingPreview = nullptr;
    QLabel *m_pendingName = nullptr;
    QLineEdit *m_captionInput = nullptr;
    QPushButton *m_pendingSendBtn = nullptr;
    QPushButton *m_pendingCancelBtn = nullptr;
    QString m_pendingFilePath;
};

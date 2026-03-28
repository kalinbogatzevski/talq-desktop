#pragma once

#include <QWidget>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
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

signals:
    void sendMessage(const QString &text);

private:
    void sendAction();

    QTextEdit *m_input = nullptr;
    QPushButton *m_sendBtn = nullptr;
    QPushButton *m_attachBtn = nullptr;
    SignalingClient *m_signaling = nullptr;
    MessageListModel *m_model = nullptr;
    QString m_topicName;
};

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
class EmojiPickerWidget;
class QListWidget;

class ComposerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit ComposerWidget(QWidget *parent = nullptr);

    void setTopicName(const QString &name);
    void setSignaling(SignalingClient *sig);
    void setMessageModel(MessageListModel *model);
    void setInputFont(const QFont &font);
    void showPendingFile(const QString &path);
    void showReplyBar(const QString &author, const QString &preview);
    void hideReplyBar();

signals:
    void sendMessage(const QString &text);
    void replyBarCancelled();

private slots:
    void sendAction();
    void confirmSendFile();
    void cancelPendingFile();
    void handleAutoreplace();
    void openEmojiPicker();
    void maybeShowCompletion();
    void applyCompletion(int row);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QTextEdit *m_input = nullptr;
    QPushButton *m_sendBtn = nullptr;
    QPushButton *m_attachBtn = nullptr;
    QPushButton *m_emojiBtn = nullptr;
    EmojiPickerWidget *m_picker = nullptr;
    SignalingClient *m_signaling = nullptr;
    MessageListModel *m_model = nullptr;
    QString m_topicName;

    // Pending file confirmation bar
    QWidget *m_pendingBar = nullptr;
    QLabel *m_pendingPreview = nullptr;
    QLabel *m_pendingName = nullptr;
    QPushButton *m_pendingCancelBtn = nullptr;
    QString m_pendingFilePath;

    // Reply bar
    QWidget *m_replyBar = nullptr;
    QLabel *m_replyLabel = nullptr;

    QListWidget *m_completion = nullptr;
};

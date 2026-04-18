#pragma once

#include <QWidget>
#include <QTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QHash>
#include <QSet>
#include <QImage>

class SignalingClient;
class MessageListModel;
class EmojiPickerWidget;
class QListWidget;
class QTimer;

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
    void setNextSendSilent(bool s) { m_nextSendSilent = s; }
    bool isNextSendSilent() const { return m_nextSendSilent; }
    void showReplyBar(const QString &author, const QString &preview);
    void hideReplyBar();
    void showEditingBar(const QString &originalText);
    void hideEditingBar();
    bool isEditing() const { return m_editing; }

signals:
    void sendMessage(const QString &text, bool silent);
    void replyBarCancelled();
    void editMessageRequested(const QString &newText);
    void editingBarCancelled();

private slots:
    void sendAction();
    void confirmSendFile();
    void cancelPendingFile();
    void handleAutoreplace();
    void openEmojiPicker();
    void maybeShowCompletion();
    void applyCompletion(int row);
    void maybeShowMentionCompletion();
    void fetchMentionsDebounced();
    void applyMentionCompletion(int row);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    bool m_nextSendSilent = false;

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

    // Editing bar
    QWidget *m_editingBar = nullptr;
    QLabel  *m_editingPreview = nullptr;
    bool     m_editing = false;

    QListWidget *m_completion = nullptr;

    // Mention popup
    QListWidget *m_mentionPopup = nullptr;
    QTimer      *m_mentionDebounce = nullptr;
    QString      m_pendingMentionQuery;
    int          m_mentionWordStart = -1;
    QHash<QString, QImage> m_mentionAvatarCache;
    QSet<QString>          m_mentionAvatarPending;

    QImage fetchMentionAvatar(const QString &userId);
    void requestMentionAvatar(const QString &userId);
};

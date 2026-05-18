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
#include "ui/TalqIconButton.h"

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

    // Composer font pixel size at zoom 1.0. MainWindow scales this by the
    // zoom factor when building the input font; ComposerWidget derives its
    // box chrome from the same value. Single source of truth so the two
    // sides cannot silently desync.
    static constexpr int kBaseInputPx = 14;

    void setTopicName(const QString &name);
    void setSignaling(SignalingClient *sig);
    void setMessageModel(MessageListModel *model);
    void setInputFont(const QFont &font);
    void showPendingFile(const QString &path);
    // Show an "encoding image…" placeholder while a pasted image is being
    // saved to disk on a worker thread. Replaced by showPendingFile() when
    // the encode finishes, or cleared by cancelPendingFile() on error.
    void showPendingEncoding();
    // Exposed so the inner ComposeTextEdit can clear the pending bar when
    // an async paste-encode fails.
    void cancelPendingFile();
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
    void userInteracted();   // user clicked/typed in composer — divider can dismiss
    // Right-click on the send button → user picked a future delivery time.
    // sendAt is an absolute unix timestamp (seconds since epoch).
    void scheduleRequested(const QString &text, qint64 sendAt, bool silent);
    // "Manage scheduled…" entry in the send button's right-click menu.
    // MainWindow opens ScheduledMessagesDialog in response.
    void manageScheduledRequested();

private slots:
    void sendAction();
    void openScheduleMenu();
    void autoResizeInput();
    void confirmSendFile();
    void handleAutoreplace();
    void flushAutoreplace();
    void openEmojiPicker();
    void maybeShowCompletion();
    void applyCompletion(int row);
    void maybeShowMentionCompletion();
    void fetchMentionsDebounced();
    void applyMentionCompletion(int row);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;   // re-theme on palette change

private:
    void applyChrome();   // composer surface + bars, palette-driven
    bool m_nextSendSilent = false;
    int  m_minInputH = 38;
    int  m_maxInputH = 160;
    // Scaled QTextEdit vertical padding; autoResizeInput() needs it to add
    // the real chrome (padding + border) on top of the document height.
    int  m_inputVPad = 8;

    QTextEdit *m_input = nullptr;
    TalqIconButton *m_sendBtn = nullptr;
    TalqIconButton *m_attachBtn = nullptr;
    TalqIconButton *m_emojiBtn = nullptr;
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

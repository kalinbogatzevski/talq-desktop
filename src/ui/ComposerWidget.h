#pragma once
#include "core/ComposerSchedulePolicy.h"
#include "core/MessageLengthPolicy.h"

#include <QAction>
#include <QDateTime>
#include <QVector>
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
class QMenu;
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
    // Same, for a file TalQ made itself (a pasted image encoded into %TEMP%): the composer owns
    // it, so it is removed if the user cancels or replaces it, and handed to the model to remove
    // after the share succeeds. Never use it for a file the user picked or dropped.
    void showPendingOwnedFile(const QString &path);
    // Show an "encoding image…" placeholder while a pasted image is being
    // saved to disk on a worker thread. Replaced by showPendingFile() when
    // the encode finishes, or cleared by cancelPendingFile() on error.
    void showPendingEncoding();
    // Exposed so the inner ComposeTextEdit can clear the pending bar when
    // an async paste-encode fails.
    void cancelPendingFile();
    // A file waiting in the composer to be sent, including a pasted image that
    // is still being prepared (its path is only known once the encode is
    // done, but the bar is already up). It is not a per-conversation draft and
    // does not survive a restart, so the auto-install gate treats it like
    // unsent text. isHidden(), not isVisible(): the bar keeps its state while
    // the whole composer is hidden on Home.
    bool hasStagedAttachment() const
    {
        return !m_pendingFilePath.isEmpty() || (m_pendingBar && !m_pendingBar->isHidden());
    }
    void setNextSendSilent(bool s) { m_nextSendSilent = s; }
    // Polls: available on the server (`talk-polls`), and the type of the open
    // conversation — 1 is one-to-one, where the server refuses polls outright.
    void setPollsAvailable(bool v) { m_pollsAvailable = v; }
    void setConversationType(int t) { m_conversationType = t; }
    // The server's longest accepted message, in characters; 0 = unknown, which never blocks.
    // Over it, Send/Edit/Schedule do nothing and the text stays in the box (an inline note says
    // by how much), instead of clearing the box into a send the server refuses with a 413.
    void setMaxMessageLength(int n);
    bool isNextSendSilent() const { return m_nextSendSilent; }

    // The part of "Send later" after a time was picked: schedules what is in the box, or does
    // nothing and returns false. Public because the menu that leads here is modal and cannot be
    // driven by a test; it is also the one place the checks live, so a state that changed while
    // the time picker was open is judged now, not when the menu was built.
    //   - refused while editing, with a file staged or being prepared, or while recording
    //     (ComposerSchedulePolicy.h): the box, the staged file and the editing bar are left
    //     exactly as they were -- the check comes before the autoreplace flush;
    //   - refused when the box is empty or over the server's limit, judged on the flushed text (a
    //     pending ":)" may have gained its trailing space, which is harmless: it is trimmed);
    //   - refused when `expectedRoomToken` is given and the model has moved to another
    //     conversation meanwhile (the box then holds THAT room's draft);
    //   - always disarms the "send silently" flag, whichever way it leaves;
    //   - empties the box BEFORE emitting scheduleRequested, because the request can be refused
    //     synchronously and handed back into the box (MainWindow's scheduleFailed handler).
    bool scheduleAt(qint64 sendAtSecs, const QString &expectedRoomToken = QString());

    // The send button's right-click menu, built but not shown. Split from openScheduleMenu() so
    // the state-dependent enabling can be asserted without exec()'ing a modal menu.
    struct ScheduleMenu {
        QAction *send = nullptr;
        QAction *silent = nullptr;            // "Send silently"; disabled where it cannot be honoured
        QAction *later = nullptr;             // the "Send later" submenu's own action
        QAction *custom = nullptr;            // "Custom time…" inside it
        QAction *manage = nullptr;
        QVector<QAction *> presets;           // parallel to presetTimes
        QVector<QDateTime> presetTimes;
    };
    ScheduleMenu buildScheduleMenu(QMenu &menu, const QDateTime &now) const;
    // Why "Send later" is not available for the box as it is right now (no side effects: the
    // box is not flushed or changed); ScheduleBlock::None when it is. The send button's menu is
    // built from this, so a test of it is a test of the menu.
    talq::ScheduleBlock scheduleBlockNow() const;
    static QString scheduleBlockHint(talq::ScheduleBlock block);

    // Trimmed contents of the input box. Empty = composer is "clean". Used
    // by the auto-install gate so a draft in flight can't be killed by a
    // background self-restart.
    QString currentText() const;
    // 0.65.3 — needed for per-conversation drafts. Neither existed before, so
    // nothing outside sendAction() could empty or prefill the composer.
    // setText() deliberately does NOT emit textChanged: restoring a draft must
    // not fire the typing indicator or count as user interaction.
    void setText(const QString &text);
    void clearText();
    void showReplyBar(const QString &author, const QString &preview);
    void hideReplyBar();
    void showEditingBar(const QString &originalText);
    void hideEditingBar();
    bool isEditing() const { return m_editing; }

    // Up-arrow on an empty composer → edit the last own message (Telegram-style).
    // Returns true if it consumed the key (composer was idle, request emitted).
    bool requestEditLast();

signals:
    void sendMessage(const QString &text, bool silent);
    void replyBarCancelled();
    void editMessageRequested(const QString &newText);
    void editLastRequested();   // up-arrow on empty composer
    void editingBarCancelled();
    void userInteracted();   // user clicked/typed in composer — divider can dismiss
    // Right-click on the send button → user picked a future delivery time.
    // sendAt is an absolute unix timestamp (seconds since epoch).
    void scheduleRequested(const QString &text, qint64 sendAt, bool silent);
    // "Manage scheduled…" entry in the send button's right-click menu.
    // MainWindow opens ScheduledMessagesDialog in response.
    void manageScheduledRequested();
    // Voice message: the composer owns the button and the timer readout, the
    // window owns the recorder and the sending. Emitted on the first click
    // (start) and the second (stop and send); cancel discards.
    void voiceRecordToggled();
    void voiceRecordCancelled();
    // The user picked "Poll…" from the attach menu; the window owns the dialog.
    void createPollRequested();

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
    // Shows or hides the "too long" note for what is in the box right now.
    void updateLengthIndicator();
    // What the server would be handed for `text` (already trimmed, as it would be sent), measured
    // the way the server measures it: a staged file's caption on its encoded form, a plain send
    // with the bot's @-mention in front where the model will add one. `plainSend` is false for a
    // scheduled message and an edit, which the model sends untouched.
    talq::msglen::Check lengthCheckFor(const QString &text, bool plainSend) const;
    // True (and the note is up to date) when `text` is longer than the server accepts. The caller
    // must then return without clearing anything.
    bool tooLongToSend(const QString &text, bool plainSend);
    int     m_maxMessageLength = 0;
    QLabel *m_lengthLabel = nullptr;
    bool m_nextSendSilent = false;
    int  m_minInputH = 38;
    int  m_maxInputH = 160;
    // Scaled QTextEdit vertical padding; autoResizeInput() needs it to add
    // the real chrome (padding + border) on top of the document height.
    int  m_inputVPad = 8;

    bool m_pollsAvailable = false;
    int  m_conversationType = 0;
    QTextEdit *m_input = nullptr;
    TalqIconButton *m_sendBtn = nullptr;
    TalqIconButton *m_attachBtn = nullptr;
    TalqIconButton *m_emojiBtn = nullptr;
    TalqIconButton *m_micBtn = nullptr;
    QLabel *m_recLabel = nullptr;      // elapsed time while recording
    TalqIconButton *m_recCancelBtn = nullptr;
    bool m_recording = false;

public:
    // Driven by MainWindow from VoiceRecorder's signals: the composer never
    // touches the microphone, it only shows what is happening.
    void setRecordingState(bool recording);
    void setRecordingElapsed(qint64 ms);
private:
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
    // m_pendingFilePath is a temp file TalQ created (showPendingOwnedFile), so the composer may
    // delete it. False for everything the user chose: file dialog, drop, pasted Explorer URL.
    bool m_pendingFileOwned = false;
    // Removes the staged file if (and only if) the composer owns it, and forgets the ownership.
    void discardOwnedPendingFile();

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

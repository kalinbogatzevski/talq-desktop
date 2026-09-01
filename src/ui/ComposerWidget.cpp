#include "ComposerWidget.h"
#include "EmojiPickerWidget.h"
#include "NextcloudFilePickerDialog.h"
#include "painter/PainterTheme.h"
#include "core/SignalingClient.h"
#include "core/EmojiData.h"
#include "core/MentionCandidate.h"
#include "models/MessageListModel.h"
#include <QFileDialog>
#include <QKeyEvent>
#include <QMessageBox>
#include <QListWidget>
#include <QMimeData>
#include <QClipboard>
#include <QApplication>
#include <QImage>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QTimer>
#include <QIcon>
#include <QNetworkReply>
#include <QPixmap>
#include <QRegularExpression>
#include <QMenu>
#include <QAction>
#include <QDialog>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QFutureWatcher>
#include <QtConcurrent>
#include <cmath>
#include <QCursor>
#include <QEvent>
#include <QPalette>

namespace {

// Shortcode / short-form characters: letters, digits, '_', '+', '-'.
// Used to walk backward from the cursor when detecting ":word[:]".
bool isShortcodeChar(QChar c)
{
    return c.isLetterOrNumber()
        || c == QLatin1Char('_')
        || c == QLatin1Char('+')
        || c == QLatin1Char('-');
}

// Walk backward from `from` (exclusive) while the char matches `isShortcodeChar`.
// Returns the index of the first char that does not match, or -1 if we walked
// past the start. Callers then check whether text[result] is ':'.
int walkShortcodeBack(const QString &text, int from)
{
    int i = from - 1;
    while (i >= 0 && isShortcodeChar(text[i]))
        --i;
    return i;
}

// Autocomplete / mention popups inherit the app-wide AppStyle QListWidget
// (theme-driven) — no local palette here any more.

// Composer text-box sizing at zoom 1.0. The QTextEdit's true vertical
// chrome is padding + border + the QTextDocument margin (top *and*
// bottom); the widget height MUST budget all of it or the zoomed text /
// placeholder gets clipped — which is exactly what the old `lineH + 16`
// (only 16 of the real ~26) did, worsening as zoom enlarged the font.
// kBaseInputPx lives in the header (ComposerWidget::kBaseInputPx) so
// MainWindow scales the same base; the rest stay local to this file.
constexpr int kBaseVPad      = 8;   // QTextEdit vertical padding @1.0
constexpr int kBaseHPad      = 16;  // QTextEdit horizontal padding @1.0
constexpr int kBaseDocMargin = 4;   // QTextDocument margin @1.0
constexpr int kInputBorder   = 1;   // hairline border: counted, never scaled

} // namespace

// Custom text edit that sends on Enter, newline on Shift+Enter, handles image paste
class ComposeTextEdit : public QTextEdit
{
public:
    ComposeTextEdit(ComposerWidget *parent) : QTextEdit(parent), m_owner(parent) {
        setAcceptRichText(false);
        setTabChangesFocus(true);
        document()->setDocumentMargin(4);
    }

    QSize sizeHint() const override { return QSize(200, 36); }
    QSize minimumSizeHint() const override { return QSize(100, 36); }

protected:
    void keyPressEvent(QKeyEvent *e) override {
        if (e->key() == Qt::Key_Escape && m_owner->isEditing()) {
            m_owner->hideEditingBar();
            e->accept();
            return;
        }
        // Up-arrow on an empty composer edits the last own message (Telegram).
        // Only when empty so multi-line cursor navigation is unaffected.
        if (e->key() == Qt::Key_Up && toPlainText().isEmpty()
            && !(e->modifiers() & (Qt::ShiftModifier | Qt::ControlModifier))) {
            if (m_owner->requestEditLast()) { e->accept(); return; }
        }
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            if (e->modifiers() & Qt::ShiftModifier) {
                QTextEdit::keyPressEvent(e);
            } else {
                e->accept();
                if (e->modifiers() & Qt::AltModifier)
                    m_owner->setNextSendSilent(true);
                QMetaObject::invokeMethod(m_owner, "sendAction", Qt::QueuedConnection);
            }
            return;
        }
        QTextEdit::keyPressEvent(e);
    }

    bool canInsertFromMimeData(const QMimeData *source) const override {
        return source->hasImage() || source->hasUrls() || QTextEdit::canInsertFromMimeData(source);
    }

    void insertFromMimeData(const QMimeData *source) override {
        if (source->hasImage()) {
            QImage img = qvariant_cast<QImage>(source->imageData());
            if (!img.isNull()) {
                const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
                const QString path = dir + QStringLiteral("/talq_paste_")
                                   + QString::number(QDateTime::currentMSecsSinceEpoch())
                                   + QStringLiteral(".png");

                // Immediate feedback — show "Preparing image…" so the user
                // knows something's happening. Large pastes used to freeze
                // the UI while PNG encoding ran on the main thread.
                m_owner->showPendingEncoding();

                auto *watcher = new QFutureWatcher<bool>(m_owner);
                QObject::connect(watcher, &QFutureWatcher<bool>::finished, m_owner,
                    [owner = m_owner, watcher, path]() {
                        const bool ok = watcher->result();
                        watcher->deleteLater();
                        if (!ok) {
                            owner->cancelPendingFile();
                            qWarning() << "paste: failed to save image to" << path;
                            QMessageBox::warning(owner, tr("Paste failed"),
                                tr("Could not save pasted image to temporary file:\n%1")
                                    .arg(path));
                            return;
                        }
                        owner->showPendingFile(path);
                    });
                watcher->setFuture(QtConcurrent::run([img, path]() {
                    return img.save(path, "PNG");
                }));
                return;
            }
        }
        if (source->hasUrls()) {
            for (const QUrl &url : source->urls()) {
                if (url.isLocalFile()) {
                    m_owner->showPendingFile(url.toLocalFile());
                    return;
                }
            }
        }
        QTextEdit::insertFromMimeData(source);
    }

private:
    ComposerWidget *m_owner;
};

ComposerWidget::ComposerWidget(QWidget *parent)
    : QWidget(parent)
{
    // Warm surface with a subtle top hairline — palette-driven, set by
    // applyChrome() at the end of the constructor (and on theme change).

    auto *layout = new QHBoxLayout();
    layout->setContentsMargins(14, 10, 14, 10);
    layout->setSpacing(8);

    // Painted call-bar chip icons (vector, theme-tinted) \u2014 one idiom with
    // the call control bar.
    m_attachBtn = new TalqIconButton(QStringLiteral("attach"), this);
    m_attachBtn->setFixedSize(38, 38);
    m_attachBtn->setToolTip(tr("Attach file"));
    layout->addWidget(m_attachBtn);

    // Voice message. Next to the paperclip because it is the same kind of
    // act -- attaching something that is not typed -- and because the two
    // never apply at once.
    m_micBtn = new TalqIconButton(QStringLiteral("mic"), this);
    m_micBtn->setFixedSize(38, 38);
    m_micBtn->setToolTip(tr("Record a voice message"));
    layout->addWidget(m_micBtn);
    connect(m_micBtn, &QAbstractButton::clicked,
            this, &ComposerWidget::voiceRecordToggled);

    // Discard. Shown only while recording, and FIRST in the row so the
    // destructive control is nowhere near the one that sends.
    m_recCancelBtn = new TalqIconButton(QStringLiteral("trash"), this);
    m_recCancelBtn->setFixedSize(34, 34);
    m_recCancelBtn->setToolTip(tr("Discard recording"));
    m_recCancelBtn->setVisible(false);
    layout->addWidget(m_recCancelBtn);
    connect(m_recCancelBtn, &QAbstractButton::clicked,
            this, &ComposerWidget::voiceRecordCancelled);

    // Elapsed time, standing in for the text field while recording.
    m_recLabel = new QLabel(this);
    m_recLabel->setVisible(false);
    layout->addWidget(m_recLabel, 1);

    m_input = new ComposeTextEdit(this);
    m_input->setPlaceholderText(tr("Write a message\u2026"));
    setFocusProxy(m_input);
    // The QTextEdit stylesheet and min/max height are owned by
    // setInputFont() (called at the end of this constructor and on every
    // zoom change) so default and zoomed state share one code path and
    // cannot disagree. The previous inline stylesheet hard-coded
    // font-size:14px, which overrides setFont() and broke composer zoom.
    layout->addWidget(m_input, 1);

    m_emojiBtn = new TalqIconButton(QStringLiteral("emoji"), this);
    m_emojiBtn->setText(QStringLiteral("\uE76E"));          // emoji face
    m_emojiBtn->setFixedSize(34, 34);
    m_emojiBtn->setToolTip(tr("Emoji"));
    layout->addWidget(m_emojiBtn);
    connect(m_emojiBtn, &QAbstractButton::clicked, this, &ComposerWidget::openEmojiPicker);

    m_sendBtn = new TalqIconButton(QStringLiteral("send"), this);
    m_sendBtn->setText(QStringLiteral("\uE724"));           // send
    m_sendBtn->setFixedSize(38, 38);
    m_sendBtn->setToolTip(tr("Send — Enter"));
    layout->addWidget(m_sendBtn);

    connect(m_sendBtn, &QAbstractButton::clicked, this, &ComposerWidget::sendAction);

    m_sendBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sendBtn, &QWidget::customContextMenuRequested,
            this, &ComposerWidget::openScheduleMenu);

    m_sendBtn->installEventFilter(this);

    connect(m_input, &QTextEdit::textChanged, this, &ComposerWidget::handleAutoreplace);
    connect(m_input, &QTextEdit::textChanged, this, &ComposerWidget::maybeShowCompletion);
    connect(m_input, &QTextEdit::textChanged, this, &ComposerWidget::maybeShowMentionCompletion);

    connect(m_input->document(), &QTextDocument::contentsChanged,
            this, &ComposerWidget::autoResizeInput);
    connect(m_input, &QTextEdit::textChanged, this, [this]() {
        emit userInteracted();
    });

    m_mentionDebounce = new QTimer(this);
    m_mentionDebounce->setSingleShot(true);
    m_mentionDebounce->setInterval(150);
    connect(m_mentionDebounce, &QTimer::timeout,
            this, &ComposerWidget::fetchMentionsDebounced);

    m_input->installEventFilter(this);
    connect(m_attachBtn, &QAbstractButton::clicked, this, [this]() {
        QMenu menu(this);
        QAction *fromDevice = menu.addAction(
            QStringLiteral("\U0001F4BB  ") + tr("From this device\u2026"));
        QAction *fromNc = menu.addAction(
            QStringLiteral("\u2601\uFE0F  ") + tr("From Nextcloud\u2026"));
        const bool canShareFromNc = m_model && m_model->api() && m_model->api()->isAuthenticated()
                                    && !m_model->conversationToken().isEmpty();
        fromNc->setEnabled(canShareFromNc);

        // Poll. ABSENT rather than disabled in a one-to-one conversation: the
        // server refuses a poll there outright — only group and public rooms
        // may have one — so offering it and failing afterwards would let
        // someone write a whole poll before being told it was never possible.
        QAction *newPoll = nullptr;
        if (m_pollsAvailable && m_conversationType != 1) {
            menu.addSeparator();
            newPoll = menu.addAction(QStringLiteral("\U0001F4CA  ") + tr("Poll…"));
        }

        QPoint pos = m_attachBtn->mapToGlobal(QPoint(0, -menu.sizeHint().height()));
        QAction *picked = menu.exec(pos);
        if (picked && picked == newPoll) { emit createPollRequested(); return; }
        if (picked == fromDevice) {
            QString file = QFileDialog::getOpenFileName(this, tr("Send file"));
            if (!file.isEmpty())
                showPendingFile(file);
        } else if (picked == fromNc) {
            auto *dlg = new NextcloudFilePickerDialog(m_model->api(), this);
            dlg->setAttribute(Qt::WA_DeleteOnClose);
            const QString convToken = m_model->conversationToken();
            connect(dlg, &QDialog::accepted, this, [this, dlg, convToken]() {
                const QString path = dlg->pickedPath();
                if (path.isEmpty() || !m_model) return;
                m_model->api()->shareNextcloudFileToChat(convToken, path, this,
                    [this](bool ok, int status, const QString &message) {
                        if (ok) return;
                        QString reason;
                        if (status == 0)        reason = tr("Couldn\u2019t reach Nextcloud \u2014 check your connection.");
                        else if (status == 401) reason = tr("Your Nextcloud session has expired.");
                        else if (status == 403) reason = tr("Sharing isn\u2019t permitted on this server.");
                        else if (status == 404) reason = tr("That file no longer exists on Nextcloud.");
                        else if (!message.isEmpty())
                            reason = tr("Nextcloud refused the share: %1").arg(message);
                        else
                            reason = tr("Nextcloud refused the share (HTTP %1).").arg(status);
                        QMessageBox::warning(this, tr("Couldn\u2019t share"), reason);
                    });
            });
            dlg->show();
        }
    });

    // ── Pending file confirmation bar (hidden by default) ──
    m_pendingBar = new QWidget(this);
    m_pendingBar->hide();
    auto *pendingLayout = new QHBoxLayout(m_pendingBar);
    pendingLayout->setContentsMargins(8, 4, 8, 4);
    pendingLayout->setSpacing(8);

    m_pendingPreview = new QLabel(m_pendingBar);
    m_pendingPreview->setObjectName("pendingPreview");
    m_pendingPreview->setFixedSize(48, 48);
    m_pendingPreview->setAlignment(Qt::AlignCenter);
    // Surface set by showPendingFile()/showPendingEncoding() (palette-driven).
    pendingLayout->addWidget(m_pendingPreview);

    m_pendingName = new QLabel(m_pendingBar);
    m_pendingName->setObjectName("pendingName");
    m_pendingName->setProperty("role", "secondary");
    pendingLayout->addWidget(m_pendingName, 1);

    m_pendingCancelBtn = new QPushButton("\u2715", m_pendingBar);
    m_pendingCancelBtn->setObjectName("pendingCancel");
    m_pendingCancelBtn->setProperty("variant", "danger");
    m_pendingCancelBtn->setFixedSize(32, 32);
    m_pendingCancelBtn->setCursor(Qt::PointingHandCursor);
    m_pendingCancelBtn->setToolTip("Cancel");
    connect(m_pendingCancelBtn, &QPushButton::clicked, this, &ComposerWidget::cancelPendingFile);
    pendingLayout->addWidget(m_pendingCancelBtn);

    // Reply bar (hidden by default)
    m_replyBar = new QWidget(this);
    m_replyBar->hide();
    m_replyBar->setObjectName("replyBar");
    m_replyBar->setFixedHeight(36);   // surface from applyChrome()
    auto *replyBarLayout = new QHBoxLayout(m_replyBar);
    replyBarLayout->setContentsMargins(12, 0, 8, 0);
    replyBarLayout->setSpacing(8);
    // Leading reply glyph replaces the banned 3px teal side-stripe.
    auto *replyIcon = new QLabel(QStringLiteral("↩"), m_replyBar);  // ↩
    replyIcon->setObjectName("replyIcon");
    replyIcon->setProperty("role", "success");
    replyBarLayout->addWidget(replyIcon);
    m_replyLabel = new QLabel(m_replyBar);
    m_replyLabel->setObjectName("replyLabel");
    m_replyLabel->setProperty("role", "secondary");
    replyBarLayout->addWidget(m_replyLabel, 1);
    auto *replyCancelBtn = new QPushButton("\u2715", m_replyBar);
    replyCancelBtn->setFixedSize(28, 28);
    replyCancelBtn->setCursor(Qt::PointingHandCursor);
    replyCancelBtn->setObjectName("replyCancel");
    replyCancelBtn->setProperty("variant", "ghost");
    connect(replyCancelBtn, &QPushButton::clicked, this, &ComposerWidget::hideReplyBar);
    replyBarLayout->addWidget(replyCancelBtn);

    // Editing bar (hidden by default)
    m_editingBar = new QWidget(this);
    m_editingBar->hide();
    m_editingBar->setObjectName("editingBar");
    m_editingBar->setFixedHeight(36);   // surface from applyChrome()
    auto *editBarLayout = new QHBoxLayout(m_editingBar);
    editBarLayout->setContentsMargins(12, 0, 8, 0);
    editBarLayout->setSpacing(8);

    auto *editIcon = new QLabel(QStringLiteral("\u270F\uFE0F"), m_editingBar); // ✏️
    editIcon->setObjectName("editIcon");
    editBarLayout->addWidget(editIcon);

    m_editingPreview = new QLabel(m_editingBar);
    m_editingPreview->setObjectName("editingPreview");
    m_editingPreview->setProperty("role", "secondary");
    editBarLayout->addWidget(m_editingPreview, 1);

    auto *editCancelBtn = new QPushButton("\u2715", m_editingBar);
    editCancelBtn->setFixedSize(28, 28);
    editCancelBtn->setCursor(Qt::PointingHandCursor);
    editCancelBtn->setObjectName("editCancel");
    editCancelBtn->setProperty("variant", "ghost");
    connect(editCancelBtn, &QPushButton::clicked, this, &ComposerWidget::hideEditingBar);
    editBarLayout->addWidget(editCancelBtn);

    // Insert bars above the input row
    auto *mainLayout = new QVBoxLayout();
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    mainLayout->addWidget(m_pendingBar);
    mainLayout->addWidget(m_replyBar);
    mainLayout->addWidget(m_editingBar);

    auto *inputRow = new QWidget(this);
    inputRow->setLayout(layout);
    mainLayout->addWidget(inputRow);
    setLayout(mainLayout);

    // Establish initial composer metrics through the SAME path zoom uses,
    // so the default state is computed with the correct chrome budget and
    // can never disagree with the zoomed state. setInputFont() also sets
    // this widget's maximum height (replaces the old fixed 280).
    QFont baseInputFont = m_input->font();
    baseInputFont.setPixelSize(kBaseInputPx);
    setInputFont(baseInputFont);

    applyChrome();
}

QString ComposerWidget::currentText() const
{
    return m_input ? m_input->toPlainText().trimmed() : QString();
}

// 0.65.3. The composer had no way to be written to or emptied from outside:
// it was cleared only in sendAction(), so switching conversations carried
// whatever was half-typed into the next room — where it could be sent to the
// wrong person. Both halves of the draft feature need this.
void ComposerWidget::setText(const QString &text)
{
    if (!m_input) return;
    // Block signals so restoring a draft does not read as the user typing:
    // textChanged drives the typing indicator (which would tell the room you
    // are typing the moment you merely opened it) and the auto-resize.
    const QSignalBlocker blocker(m_input);
    m_input->setPlainText(text);
    // Caret to the end, so the user continues the draft rather than typing in
    // front of it.
    QTextCursor c = m_input->textCursor();
    c.movePosition(QTextCursor::End);
    m_input->setTextCursor(c);
    // Height still has to follow the restored content — that is a layout
    // concern, not a "user typed" one, so it is called directly.
    autoResizeInput();
}

void ComposerWidget::clearText()
{
    setText(QString());
}

void ComposerWidget::applyChrome()
{
    // Composer surface + the reply/edit/pending bars — palette-driven so
    // all four themes track from one place (the app palette = PainterTheme).
    const QPalette p = palette();
    const QString bg   = p.color(QPalette::Window).name();
    const QString bar  = p.color(QPalette::Base).name();
    const QString line = p.color(QPalette::Mid).name();
    setStyleSheet(QString(
        "ComposerWidget { background: %1; border-top: 1px solid %2; }"
        "QWidget#replyBar, QWidget#editingBar {"
        "  background: %3; border-top: 1px solid %2; }")
        .arg(bg, line, bar));
}

void ComposerWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // ApplicationPaletteChange (NOT PaletteChange): the latter is re-emitted
    // by our own setStyleSheet() inside applyChrome() → infinite recursion.
    if (event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::ThemeChange)
        applyChrome();
}

// Reserve vertical space above the input row for any combination of the
// pending-file / reply / editing bars (each is 36-56px; total ≤120).
static constexpr int kComposerBarsReserve = 120;

void ComposerWidget::autoResizeInput()
{
    if (!m_input) return;
    // document()->size() already includes the (scaled) document margin.
    // It does NOT include the stylesheet padding + border, and
    // frameWidth() is 0 when the border is drawn by a stylesheet — so the
    // old `frame + 4` under-counted and clipped lower lines while typing.
    const int docH   = int(std::ceil(m_input->document()->size().height()));
    const int chrome = 2 * (m_inputVPad + kInputBorder);
    const int newH   = qBound(m_minInputH, docH + chrome, m_maxInputH);
    if (m_input->height() != newH)
        m_input->setFixedHeight(newH);
}

void ComposerWidget::setTopicName(const QString &name)
{
    m_topicName = name;
    if (name.isEmpty())
        m_input->setPlaceholderText(tr("Message…"));
    else
        m_input->setPlaceholderText(tr("Reply in %1…").arg(name));
}

void ComposerWidget::setSignaling(SignalingClient *sig)
{
    m_signaling = sig;
}

void ComposerWidget::setMessageModel(MessageListModel *model)
{
    m_model = model;
    if (m_model) {
        // Hide autocomplete popups and clear pending state whenever the
        // active conversation changes — prevents stale responses from one
        // room surfacing candidates in another.
        connect(m_model, &MessageListModel::conversationTokenChanged,
                this, [this]() {
            if (m_mentionPopup) m_mentionPopup->hide();
            if (m_completion) m_completion->hide();
            m_mentionWordStart = -1;
            m_pendingMentionQuery.clear();
            if (m_mentionDebounce) m_mentionDebounce->stop();
            hideEditingBar();
        });
    }
}

void ComposerWidget::setInputFont(const QFont &font)
{
    m_input->setFont(font);

    const int   px    = font.pixelSize() > 0 ? font.pixelSize() : kBaseInputPx;
    const qreal scale = qreal(px) / kBaseInputPx;
    const int   vpad  = qRound(kBaseVPad      * scale);
    const int   hpad  = qRound(kBaseHPad      * scale);
    const int   docM  = qRound(kBaseDocMargin * scale);
    m_inputVPad = vpad;
    m_input->document()->setDocumentMargin(docM);

    // ONLY zoom-scaled geometry here. Colours (background, text, border,
    // selection, :focus) come from AppStyle's app-wide QTextEdit rule,
    // which is generated from PainterTheme — correct on every theme and
    // re-applied on theme change. The previous version baked colours from
    // palette() at construction time, before the theme palette was set,
    // so the input rendered black-text-on-black (the transparent viewport
    // shows the themed composer background behind the not-yet-themed
    // baked text). Don't reintroduce a palette()-baked colour here.
    // kInputBorder (1px) matches AppStyle's border so the height math
    // below stays exact. Viewport stays transparent so the QTextEdit's
    // rounded themed background isn't squared off by the inner viewport.
    m_input->setStyleSheet(QString(
        "QTextEdit { border-radius: %3px; padding: %1px %2px; }"
        "QTextEdit > QWidget { background: transparent; }")
        .arg(QString::number(vpad), QString::number(hpad),
             QString::number(PainterTheme::radiusControl)));

    const int lineH = QFontMetrics(font).height();
    // Full vertical chrome the widget must contain so one line of text
    // (and the placeholder) is never clipped: padding + border + document
    // margin, top *and* bottom. The old code budgeted only +16 of the
    // real ~26, so the bottom of the glyphs was cut once zoom enlarged
    // the font enough to use the line box's slack.
    const int chromeV = 2 * (vpad + kInputBorder + docM);
    m_minInputH = lineH + chromeV;
    m_maxInputH = lineH * 5 + chromeV;
    m_input->setMinimumHeight(m_minInputH);
    m_input->setMaximumHeight(m_maxInputH);
    m_sendBtn->setFixedSize(m_minInputH, m_minInputH);
    m_attachBtn->setFixedSize(m_minInputH, m_minInputH);
    setMaximumHeight(m_maxInputH + kComposerBarsReserve);
    autoResizeInput();
}

void ComposerWidget::showPendingFile(const QString &path)
{
    m_pendingFilePath = path;
    QFileInfo fi(path);
    m_pendingName->setText(fi.fileName());

    // Clear any "Preparing image…" styling left from showPendingEncoding().
    m_pendingPreview->setText(QString());
    m_pendingPreview->setStyleSheet(
        QStringLiteral("background: %1; border-radius: 6px;")
            .arg(palette().color(QPalette::Base).name()));

    QImage img(path);
    if (!img.isNull()) {
        QPixmap pix = QPixmap::fromImage(img.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_pendingPreview->setPixmap(pix);
    } else {
        m_pendingPreview->setText("\U0001F4C4");
        m_pendingPreview->setStyleSheet(
            QStringLiteral("background: %1; border-radius: 6px; font-size: 24px;")
                .arg(palette().color(QPalette::Base).name()));
    }

    m_pendingBar->show();
    m_input->setPlaceholderText(tr("Add a caption\u2026"));
    m_input->setFocus();
}

void ComposerWidget::showPendingEncoding()
{
    m_pendingFilePath.clear();
    m_pendingPreview->setPixmap(QPixmap());
    m_pendingPreview->setText(QStringLiteral("\u23F3"));                       // ⏳ hourglass
    m_pendingPreview->setStyleSheet(
        QStringLiteral("background: %1; border-radius: 6px; color: %2;"
                       " font-size: 22px;")
            .arg(palette().color(QPalette::Base).name(),
                 palette().color(QPalette::Highlight).name()));
    m_pendingName->setText(tr("Preparing image\u2026"));
    m_pendingBar->show();
}

void ComposerWidget::confirmSendFile()
{
    if (m_pendingFilePath.isEmpty()) {
        qWarning() << "confirmSendFile: no pending file path";
        return;
    }
    if (!m_model) {
        qWarning() << "confirmSendFile: no model";
        return;
    }
    QString caption = m_input->toPlainText().trimmed();
    qDebug() << "confirmSendFile: path=" << m_pendingFilePath << "caption=" << caption;
    m_model->sendFileWithCaption(m_pendingFilePath, caption);
    m_input->clear();
    cancelPendingFile();
}

void ComposerWidget::cancelPendingFile()
{
    m_pendingFilePath.clear();
    m_pendingBar->hide();
    m_input->setPlaceholderText(m_topicName.isEmpty()
        ? tr("Message…")
        : tr("Reply in %1…").arg(m_topicName));
    m_input->setFocus();
}

void ComposerWidget::showReplyBar(const QString &author, const QString &preview)
{
    m_replyLabel->setText(QStringLiteral("<span style='color:%1; font-weight:600;'>%2</span>  %3")
        .arg(palette().color(QPalette::Highlight).name(),
             author.toHtmlEscaped(), preview.toHtmlEscaped()));
    m_replyLabel->setTextFormat(Qt::RichText);
    m_replyBar->show();
}

void ComposerWidget::hideReplyBar()
{
    m_replyBar->hide();
    emit replyBarCancelled();
}

void ComposerWidget::showEditingBar(const QString &originalText)
{
    // Editing and replying are mutually exclusive.
    if (m_replyBar && m_replyBar->isVisible())
        hideReplyBar();

    QString preview = originalText;
    preview.replace(QLatin1Char('\n'), QLatin1Char(' '));
    if (preview.size() > 120) preview = preview.left(120) + QStringLiteral("…");
    m_editingPreview->setText(preview.toHtmlEscaped());
    m_editingBar->show();

    m_input->setPlainText(originalText);
    QTextCursor c = m_input->textCursor();
    c.movePosition(QTextCursor::End);
    m_input->setTextCursor(c);
    m_input->setFocus();
    m_editing = true;
}

void ComposerWidget::hideEditingBar()
{
    if (!m_editing && (!m_editingBar || !m_editingBar->isVisible())) return;
    m_editing = false;
    m_editingBar->hide();
    m_input->clear();
    emit editingBarCancelled();
}

bool ComposerWidget::requestEditLast()
{
    // Don't hijack Up while already editing or composing a reply.
    if (m_editing) return false;
    if (m_replyBar && m_replyBar->isVisible()) return false;
    emit editLastRequested();   // MainWindow loads the last own message, if any
    return true;
}

void ComposerWidget::sendAction()
{
    // If a file is pending, Enter sends the file (with caption from composer)
    if (!m_pendingFilePath.isEmpty()) {
        confirmSendFile();
        return;
    }
    // Flush pending :)/:shortcode: substitutions — handleAutoreplace normally
    // requires a trailing space, which isn't there when sending via Enter.
    flushAutoreplace();
    QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty()) return;
    if (m_signaling) m_signaling->sendStoppedTyping();

    if (m_editing) {
        emit editMessageRequested(text);
        hideEditingBar();
        return;
    }

    bool silent = m_nextSendSilent;
    m_nextSendSilent = false;
    emit sendMessage(text, silent);

    if (silent) {
        // Brief "sent silently" confirmation: flash a bell-off glyph.
        m_sendBtn->setIconId(QStringLiteral("belloff"));
        QTimer::singleShot(600, this, [this]() {
            if (m_sendBtn) m_sendBtn->setIconId(QStringLiteral("send"));
        });
    }

    m_input->clear();
}

void ComposerWidget::openScheduleMenu()
{
    QMenu menu(this);
    QFont boldF = menu.font(); boldF.setBold(true);

    // Primary actions: send now / send silent — same as the legacy menu, so
    // a right-click is still the discoverable path for "send silently".
    QAction *sendAct   = menu.addAction(tr("Send"));
    sendAct->setFont(boldF);
    QAction *silentAct = menu.addAction(QStringLiteral("\U0001F515  ")
        + tr("Send silently") + QStringLiteral(" — ") + tr("no notification"));

    menu.addSeparator();

    QMenu *later = menu.addMenu(QStringLiteral("⏰  ") + tr("Send later"));

    // Build the same quick presets as the message-level "Remind me" submenu
    // so users see a consistent vocabulary. All are emitted as absolute unix
    // seconds — the server is the source of truth for timing.
    const QDateTime now = QDateTime::currentDateTime();
    struct Preset { QString label; QDateTime when; };
    QVector<Preset> presets;
    presets.push_back({tr("In 1 hour"),       now.addSecs(60 * 60)});
    presets.push_back({tr("In 3 hours"),      now.addSecs(3 * 60 * 60)});
    {
        QDateTime tomorrow = now.addDays(1);
        tomorrow.setTime(QTime(8, 0));
        presets.push_back({tr("Tomorrow 8:00"), tomorrow});
    }
    {
        QDateTime tomorrowEve = now.addDays(1);
        tomorrowEve.setTime(QTime(18, 0));
        presets.push_back({tr("Tomorrow 18:00"), tomorrowEve});
    }
    {
        // Next Monday at 09:00 — addDays(7 - dayOfWeek() + 1) jumps from
        // any weekday to the upcoming Monday. dayOfWeek() returns 1 (Mon)
        // through 7 (Sun).
        QDateTime nextMon = now.addDays(7 - now.date().dayOfWeek() + 1);
        nextMon.setTime(QTime(9, 0));
        presets.push_back({tr("Next Monday 9:00"), nextMon});
    }
    QVector<QAction *> presetActs;
    for (const auto &p : presets) {
        QString label = QStringLiteral("%1  —  %2")
            .arg(p.label, p.when.toString(QStringLiteral("ddd dd MMM, HH:mm")));
        presetActs.push_back(later->addAction(label));
    }
    later->addSeparator();
    QAction *customAct = later->addAction(tr("Custom time…"));

    menu.addSeparator();
    QAction *manageAct = menu.addAction(QStringLiteral("\U0001F4CB  ") + tr("Manage scheduled…"));

    QAction *picked = menu.exec(QCursor::pos());
    if (!picked) return;

    if (picked == sendAct) { sendAction(); return; }
    if (picked == silentAct) { m_nextSendSilent = true; sendAction(); return; }
    if (picked == manageAct) { emit manageScheduledRequested(); return; }

    // Below: a scheduling branch. We resolve to a sendAt unix timestamp and
    // emit scheduleRequested. The actual API call lives in MainWindow so it
    // can attach the current reply context and react to the response toast.
    QDateTime sendAt;
    if (picked == customAct) {
        QDialog dlg(this);
        dlg.setWindowTitle(tr("Send at"));
        auto *layout = new QVBoxLayout(&dlg);
        auto *edit = new QDateTimeEdit(now.addSecs(60 * 60), &dlg);
        edit->setCalendarPopup(true);
        edit->setMinimumDateTime(now.addSecs(60));  // at least a minute out
        edit->setDisplayFormat(QStringLiteral("ddd dd MMM yyyy   HH:mm"));
        layout->addWidget(edit);
        auto *buttons = new QDialogButtonBox(
            QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
        layout->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        if (dlg.exec() != QDialog::Accepted) return;
        sendAt = edit->dateTime();
    } else {
        int idx = presetActs.indexOf(picked);
        if (idx < 0) return;
        sendAt = presets[idx].when;
    }

    // Reuse the same flush-and-grab dance as sendAction so :)/:shortcode:
    // gets substituted even when the user never typed a trailing space.
    flushAutoreplace();
    QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty()) return;

    bool silent = m_nextSendSilent;
    m_nextSendSilent = false;
    if (m_signaling) m_signaling->sendStoppedTyping();
    emit scheduleRequested(text, sendAt.toSecsSinceEpoch(), silent);
    m_input->clear();
}

void ComposerWidget::openEmojiPicker()
{
    if (!m_picker) {
        m_picker = new EmojiPickerWidget(this->window());
        m_picker->setWindowFlags(Qt::Popup);
        connect(m_picker, &EmojiPickerWidget::emojiSelected, this,
                [this](const QString &cp) {
            m_input->insertPlainText(cp);
            m_picker->close();
        });
        connect(m_picker, &EmojiPickerWidget::cancelled, this,
                [this]() { m_picker->close(); });
    }
    QPoint anchor = m_emojiBtn->mapToGlobal(QPoint(0, 0));
    m_picker->move(anchor.x() - m_picker->width() + m_emojiBtn->width(),
                   anchor.y() - m_picker->height() - 4);
    m_picker->show();
    m_picker->raise();
    m_picker->activateWindow();
}

void ComposerWidget::handleAutoreplace()
{
    QTextCursor cur = m_input->textCursor();
    int pos = cur.position();
    if (pos < 3) return;  // need at least ":) " or similar

    QString text = m_input->toPlainText();
    // Only trigger on trailing space (or newline).
    if (pos == 0 || !(text[pos - 1] == QLatin1Char(' ') || text[pos - 1] == QLatin1Char('\n'))) return;

    // Look back up to 10 chars for a short-form before the space.
    int start = qMax(0, pos - 11);
    QString tail = text.mid(start, pos - 1 - start);

    // Try short-form candidates (2–3 chars, e.g. ":)", ":-D", "<3").
    // Include the trailing space in the selection and re-insert it so the
    // cursor lands past the space — otherwise the next char is typed between
    // the emoji and the space.
    for (int n = qMin(3, tail.size()); n >= 2; --n) {
        QString cand = tail.right(n);
        const auto *e = EmojiData::findByShortForm(cand);
        if (e) {
            QTextCursor c = m_input->textCursor();
            c.beginEditBlock();
            c.setPosition(pos - 1 - n);
            c.setPosition(pos, QTextCursor::KeepAnchor);
            c.insertText(e->codepoints + QStringLiteral(" "));
            c.endEditBlock();
            return;
        }
    }

    // Try :shortcode: — scan back for a matching ':' pair.
    int colonEnd = pos - 1;          // the space we just typed sits at pos-1
    int wordEnd  = colonEnd - 1;     // last char of shortcode, should be ':'
    if (wordEnd < 1 || text[wordEnd] != QLatin1Char(':')) return;

    int wordStart = walkShortcodeBack(text, wordEnd);
    if (wordStart < 0 || text[wordStart] != QLatin1Char(':')) return;

    QString shortcode = text.mid(wordStart, wordEnd - wordStart + 1); // ":word:"
    const auto *e = EmojiData::findByShortcode(shortcode);
    if (!e) return;

    QTextCursor c = m_input->textCursor();
    c.beginEditBlock();
    c.setPosition(wordStart);
    c.setPosition(pos, QTextCursor::KeepAnchor);  // include trailing space
    c.insertText(e->codepoints + QStringLiteral(" "));
    c.endEditBlock();
}

void ComposerWidget::flushAutoreplace()
{
    QString text = m_input->toPlainText();
    if (text.isEmpty()) return;
    QChar last = text[text.size() - 1];
    if (last == QLatin1Char(' ') || last == QLatin1Char('\n')) return;

    // Append a synthetic space at the end and move the cursor there, so the
    // existing handleAutoreplace() logic (which keys off a trailing space)
    // sees the short-form/:shortcode:. trimmed() in sendAction drops the
    // trailing space afterwards.
    QTextCursor c = m_input->textCursor();
    c.movePosition(QTextCursor::End);
    c.insertText(QStringLiteral(" "));
    handleAutoreplace();
}

void ComposerWidget::maybeShowCompletion()
{
    QTextCursor cur = m_input->textCursor();
    int pos = cur.position();
    QString text = m_input->toPlainText();

    // Look backward for ':' introducing a partial shortcode.
    int i = walkShortcodeBack(text, pos);
    if (i < 0 || text[i] != QLatin1Char(':')) {
        if (m_completion) m_completion->hide();
        return;
    }
    QString word = text.mid(i + 1, pos - i - 1);
    if (word.length() < 2) {
        if (m_completion) m_completion->hide();
        return;
    }

    auto hits = EmojiData::search(word);
    if (hits.isEmpty()) { if (m_completion) m_completion->hide(); return; }

    if (!m_completion) {
        // Match the mention popup — non-focus-stealing so the composer keeps
        // receiving characters while the user refines the shortcode.
        m_completion = new QListWidget(this->window());
        m_completion->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool
                                    | Qt::WindowDoesNotAcceptFocus);
        m_completion->setAttribute(Qt::WA_ShowWithoutActivating);
        m_completion->setFocusPolicy(Qt::NoFocus);
        // QListWidget chrome from app-wide AppStyle (theme-driven).
        connect(m_completion, &QListWidget::itemClicked, this,
                [this](QListWidgetItem *it) { applyCompletion(m_completion->row(it)); });
        connect(m_completion, &QListWidget::itemActivated, this,
                [this](QListWidgetItem *it) { applyCompletion(m_completion->row(it)); });
    }
    m_completion->clear();
    int limit = qMin(6, int(hits.size()));
    for (int k = 0; k < limit; ++k) {
        const auto *e = hits[k];
        QString label = (e->shortcodes.isEmpty() ? e->name : e->shortcodes.first());
        auto *it = new QListWidgetItem(QIcon(EmojiData::pixmapFor(e->codepoints, 18)),
                                       QString("%1  %2").arg(e->codepoints, label));
        it->setData(Qt::UserRole, QVariant::fromValue<quintptr>(reinterpret_cast<quintptr>(e)));
        m_completion->addItem(it);
    }
    m_completion->setCurrentRow(0);

    QPoint p = m_input->mapToGlobal(m_input->rect().topLeft());
    m_completion->resize(260, 24 * limit + 8);
    m_completion->move(p.x(), p.y() - m_completion->height() - 4);
    m_completion->show();
}

void ComposerWidget::applyCompletion(int row)
{
    if (!m_completion || row < 0 || row >= m_completion->count()) return;
    auto *it = m_completion->item(row);
    const auto *e = reinterpret_cast<const EmojiData::EmojiEntry*>(
        it->data(Qt::UserRole).value<quintptr>());
    if (!e) return;

    QString text = m_input->toPlainText();
    int pos = m_input->textCursor().position();
    int i = walkShortcodeBack(text, pos);
    if (i < 0 || text[i] != QLatin1Char(':')) { m_completion->hide(); return; }

    QTextCursor c = m_input->textCursor();
    c.beginEditBlock();
    c.setPosition(i);
    c.setPosition(pos, QTextCursor::KeepAnchor);
    c.insertText(e->codepoints + QStringLiteral(" "));
    c.endEditBlock();
    EmojiData::pushRecent(e);
    m_completion->hide();
}

void ComposerWidget::maybeShowMentionCompletion()
{
    QTextCursor cur = m_input->textCursor();
    int pos = cur.position();
    QString text = m_input->toPlainText();

    // Walk backward looking for '@'.
    int i = pos - 1;
    while (i >= 0) {
        QChar c = text[i];
        if (c == QLatin1Char('@')) break;
        bool ok = c.isLetterOrNumber()
               || c == QLatin1Char('.') || c == QLatin1Char('_')
               || c == QLatin1Char('+') || c == QLatin1Char('-');
        if (!ok) { i = -1; break; }
        --i;
    }
    if (i < 0) {
        if (m_mentionPopup) m_mentionPopup->hide();
        m_mentionWordStart = -1;
        return;
    }
    // '@' must be start-of-text or after whitespace (else it's an email fragment).
    if (i > 0) {
        QChar prev = text[i - 1];
        if (!prev.isSpace()) {
            if (m_mentionPopup) m_mentionPopup->hide();
            m_mentionWordStart = -1;
            return;
        }
    }
    // If the shortcode popup is visible, mentions yield.
    if (m_completion && m_completion->isVisible()) {
        if (m_mentionPopup) m_mentionPopup->hide();
        m_mentionWordStart = -1;
        return;
    }

    m_mentionWordStart = i;
    QString partial = text.mid(i + 1, pos - i - 1);
    m_pendingMentionQuery = partial;
    m_mentionDebounce->start();
}
QImage ComposerWidget::fetchMentionAvatar(const QString &userId)
{
    if (userId.isEmpty() || userId == QStringLiteral("all"))
        return QImage();
    auto it = m_mentionAvatarCache.constFind(userId);
    if (it != m_mentionAvatarCache.constEnd()) return it.value();
    requestMentionAvatar(userId);
    return QImage();
}

void ComposerWidget::requestMentionAvatar(const QString &userId)
{
    if (m_mentionAvatarPending.contains(userId)) return;
    auto *api = m_model ? m_model->api() : nullptr;
    if (!api) return;
    m_mentionAvatarPending.insert(userId);

    QString path = QStringLiteral("/index.php/avatar/") + userId + QStringLiteral("/32");
    QNetworkReply *reply = api->getAbsoluteUrl(path);
    connect(reply, &QNetworkReply::finished, this, [this, reply, userId]() {
        reply->deleteLater();
        m_mentionAvatarPending.remove(userId);
        if (reply->error() != QNetworkReply::NoError) {
            m_mentionAvatarCache[userId] = QImage();
            return;
        }
        QImage img;
        if (!img.loadFromData(reply->readAll())) {
            m_mentionAvatarCache[userId] = QImage();
            return;
        }
        m_mentionAvatarCache[userId] = img;

        // Live-update visible popup rows.
        if (m_mentionPopup && m_mentionPopup->isVisible()) {
            for (int i = 0; i < m_mentionPopup->count(); ++i) {
                QListWidgetItem *it = m_mentionPopup->item(i);
                if (it->data(Qt::UserRole + 1).toString() == userId) {
                    it->setIcon(QIcon(QPixmap::fromImage(img).scaled(
                        24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                }
            }
        }
    });
}

void ComposerWidget::fetchMentionsDebounced()
{
    if (m_mentionWordStart < 0) return;
    auto *api = m_model ? m_model->api() : nullptr;
    if (!api) return;
    QString token = m_model ? m_model->conversationToken() : QString();
    if (token.isEmpty()) return;

    QString query = m_pendingMentionQuery;
    api->fetchMentions(token, query, this,
        [this, api, query, token](const QVector<MentionCandidate> &serverCandidates) {
            if (m_mentionWordStart < 0) return;
            if (m_pendingMentionQuery != query) return;
            if (!m_model || m_model->conversationToken() != token) return;

            // Also fetch bots enabled in this room and merge them as
            // mention candidates. NC's /mentions endpoint doesn't always
            // include bots, and the user explicitly wants to @-address them.
            api->fetchEnabledBots(token, this,
                [this, query, token, serverCandidates](bool botOk, const QVector<BotInfo> &bots) {
                    if (m_mentionWordStart < 0) return;
                    if (m_pendingMentionQuery != query) return;
                    if (!m_model || m_model->conversationToken() != token) return;

                    QVector<MentionCandidate> candidates = serverCandidates;
                    if (botOk) {
                        for (const BotInfo &b : bots) {
                            if (!b.isEnabled()) continue;
                            if (!query.isEmpty()
                                && !b.name.contains(query, Qt::CaseInsensitive))
                                continue;
                            MentionCandidate mc;
                            mc.id = b.name;
                            mc.label = b.name + QStringLiteral(" (bot)");
                            candidates.append(mc);
                        }
                    }

                    if (candidates.isEmpty()) {
                        if (m_mentionPopup) m_mentionPopup->hide();
                        return;
                    }

                    if (!m_mentionPopup) {
                        // Non-focus-stealing popup — Qt::Popup would grab
                        // keyboard input and the composer would stop receiving
                        // characters. Tool + WindowDoesNotAcceptFocus
                        // + WA_ShowWithoutActivating keeps the QTextEdit
                        // focused while the popup floats above.
                        m_mentionPopup = new QListWidget(this->window());
                        m_mentionPopup->setWindowFlags(Qt::FramelessWindowHint | Qt::Tool
                                                      | Qt::WindowDoesNotAcceptFocus);
                        m_mentionPopup->setAttribute(Qt::WA_ShowWithoutActivating);
                        m_mentionPopup->setFocusPolicy(Qt::NoFocus);
                        // List chrome from app-wide AppStyle (theme-driven).
                        m_mentionPopup->setIconSize(QSize(24, 24));
                        connect(m_mentionPopup, &QListWidget::itemClicked, this,
                                [this](QListWidgetItem *it) {
                                    applyMentionCompletion(m_mentionPopup->row(it));
                                });
                        connect(m_mentionPopup, &QListWidget::itemActivated, this,
                                [this](QListWidgetItem *it) {
                                    applyMentionCompletion(m_mentionPopup->row(it));
                                });
                    }
                    m_mentionPopup->clear();

                    int limit = qMin(6, int(candidates.size()));
                    for (int k = 0; k < limit; ++k) {
                        const MentionCandidate &c = candidates[k];
                        QString primary = c.label.isEmpty() ? c.id : c.label;
                        QString rowText;
                        if (c.id == QStringLiteral("all")) {
                            rowText = QStringLiteral("Everyone\n@all");
                        } else {
                            rowText = QStringLiteral("%1\n@%2").arg(primary, c.id);
                        }
                        auto *item = new QListWidgetItem(rowText);
                        item->setData(Qt::UserRole, c.id);
                        item->setData(Qt::UserRole + 1, c.id);
                        item->setData(Qt::UserRole + 2, c.label);
                        item->setData(Qt::UserRole + 3, int(c.source));

                        if (c.id != QStringLiteral("all")) {
                            QImage img = fetchMentionAvatar(c.id);
                            if (!img.isNull()) {
                                item->setIcon(QIcon(QPixmap::fromImage(img).scaled(
                                    24, 24, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
                            }
                        }
                        m_mentionPopup->addItem(item);
                    }
                    m_mentionPopup->setCurrentRow(0);

                    QPoint p = m_input->mapToGlobal(m_input->rect().topLeft());
                    m_mentionPopup->resize(300, 44 * limit + 8);
                    m_mentionPopup->move(p.x(), p.y() - m_mentionPopup->height() - 4);
                    m_mentionPopup->show();
                    m_mentionPopup->raise();
                });
        });
}

void ComposerWidget::applyMentionCompletion(int row)
{
    if (!m_mentionPopup || row < 0 || row >= m_mentionPopup->count()) return;
    if (m_mentionWordStart < 0) return;

    QListWidgetItem *it = m_mentionPopup->item(row);
    QString id = it->data(Qt::UserRole).toString();
    if (id.isEmpty()) { m_mentionPopup->hide(); return; }

    QString replacement;
    if (id == QStringLiteral("all")) {
        replacement = QStringLiteral("@all ");
    } else {
        static const QRegularExpression simple(
            QStringLiteral("^[A-Za-z0-9._-]+$"));
        if (simple.match(id).hasMatch())
            replacement = QStringLiteral("@") + id + QStringLiteral(" ");
        else
            replacement = QStringLiteral("@\"") + id + QStringLiteral("\" ");
    }

    int start = m_mentionWordStart;
    int end = m_input->textCursor().position();
    QTextCursor c = m_input->textCursor();
    c.beginEditBlock();
    c.setPosition(start);
    c.setPosition(end, QTextCursor::KeepAnchor);
    c.insertText(replacement);
    c.endEditBlock();

    m_mentionPopup->hide();
    m_mentionWordStart = -1;
}

bool ComposerWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_sendBtn && event->type() == QEvent::MouseButtonRelease) {
        auto *me = static_cast<QMouseEvent*>(event);
        if (me->modifiers() & Qt::AltModifier) {
            m_nextSendSilent = true;
            sendAction();
            return true;
        }
    }
    if (watched == m_input
        && (event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::FocusIn)) {
        emit userInteracted();
    }
    // Composer lost focus (user clicked the chat list / message / elsewhere
    // in TalQ) — dismiss both popups. Safe because the popups don't accept
    // focus, so clicking ON a popup item doesn't trigger this branch.
    if (watched == m_input && event->type() == QEvent::FocusOut) {
        if (m_mentionPopup && m_mentionPopup->isVisible()) {
            m_mentionPopup->hide();
            m_mentionWordStart = -1;
        }
        if (m_completion && m_completion->isVisible())
            m_completion->hide();
    }
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto *k = static_cast<QKeyEvent*>(event);

        // Mention popup first (if visible).
        if (m_mentionPopup && m_mentionPopup->isVisible()) {
            switch (k->key()) {
            case Qt::Key_Up:
                m_mentionPopup->setCurrentRow(
                    qMax(0, m_mentionPopup->currentRow() - 1));
                return true;
            case Qt::Key_Down:
                m_mentionPopup->setCurrentRow(
                    qMin(m_mentionPopup->count() - 1,
                         m_mentionPopup->currentRow() + 1));
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Tab:
                applyMentionCompletion(m_mentionPopup->currentRow());
                return true;
            case Qt::Key_Escape:
                m_mentionPopup->hide();
                m_mentionWordStart = -1;
                return true;
            default:
                break;
            }
        }

        // Shortcode popup (existing behavior).
        if (m_completion && m_completion->isVisible()) {
            switch (k->key()) {
            case Qt::Key_Up:
                m_completion->setCurrentRow(
                    qMax(0, m_completion->currentRow() - 1));
                return true;
            case Qt::Key_Down:
                m_completion->setCurrentRow(
                    qMin(m_completion->count() - 1,
                         m_completion->currentRow() + 1));
                return true;
            case Qt::Key_Return:
            case Qt::Key_Enter:
            case Qt::Key_Tab:
                applyCompletion(m_completion->currentRow());
                return true;
            case Qt::Key_Escape:
                m_completion->hide();
                return true;
            default:
                break;
            }
        }
    }
    return QWidget::eventFilter(watched, event);
}

// While recording, the composer stops being a text field: there is nothing to
// type and the only two useful acts are "send this" and "throw it away". The
// text input, emoji and send controls are hidden rather than disabled, so
// there is no greyed-out furniture to read past.
void ComposerWidget::setRecordingState(bool recording)
{
    if (m_recording == recording)
        return;
    m_recording = recording;

    m_input->setVisible(!recording);
    m_emojiBtn->setVisible(!recording);
    m_sendBtn->setVisible(!recording);
    // Attach stays VISIBLE (just disabled) so the button beside it does not
    // move. Hiding it re-packed the row and slid the record control ~120 px
    // left the instant recording began -- so the place you pressed to start
    // was not the place you had to press to stop, and the discard button had
    // slid under your cursor instead. A toggle that relocates itself between
    // its two states is a trap, not a control.
    m_attachBtn->setEnabled(!recording);
    m_recLabel->setVisible(recording);
    m_recCancelBtn->setVisible(recording);

    // The mic turns into the send-this control. Danger colouring is
    // deliberately NOT used: stopping a recording is not destructive, the
    // trash button beside it is.
    m_micBtn->setIconId(recording ? QStringLiteral("send") : QStringLiteral("mic"));
    m_micBtn->setToolTip(recording ? tr("Send voice message")
                                   : tr("Record a voice message"));
    if (recording)
        setRecordingElapsed(0);
    else
        m_input->setFocus();
}

void ComposerWidget::setRecordingElapsed(qint64 ms)
{
    if (!m_recLabel)
        return;
    const qint64 total = ms / 1000;
    // A painted disc, not a bullet glyph -- same reason as everywhere else in
    // this app: the glyph's size and baseline move with the font.
    m_recLabel->setText(QStringLiteral("●  %1:%2")
        .arg(total / 60).arg(total % 60, 2, 10, QLatin1Char('0')));
}

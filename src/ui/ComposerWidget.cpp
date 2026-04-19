#include "ComposerWidget.h"
#include "EmojiPickerWidget.h"
#include "NextcloudFilePickerDialog.h"
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
#include <cmath>
#include <QCursor>

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

// Shared stylesheet for shortcode and mention autocomplete popups.
constexpr auto kPopupStyle =
    "QListWidget { background: #222230; color: #eee; border: 1px solid #333; border-radius: 6px; }"
    "QListWidget::item { padding: 4px 8px; }"
    "QListWidget::item:selected { background: #3a3a55; }";

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
                // Save to temp file and send via model
                QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
                QString path = dir + "/talq_paste_" + QString::number(QDateTime::currentMSecsSinceEpoch()) + ".png";
                if (!img.save(path, "PNG")) {
                    qWarning() << "paste: failed to save image to" << path;
                    QMessageBox::warning(m_owner, tr("Paste failed"),
                        tr("Could not save pasted image to temporary file:\n%1").arg(path));
                    return;
                }
                m_owner->showPendingFile(path);
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
    // Unified composer background
    setStyleSheet("ComposerWidget { background: #1a1a18; border-top: 1px solid #2a2a26; }");

    auto *layout = new QHBoxLayout();
    layout->setContentsMargins(12, 8, 12, 8);
    layout->setSpacing(8);

    m_attachBtn = new QPushButton(this);
    m_attachBtn->setText(QString::fromUtf8("\xF0\x9F\x93\x8E"));  // clip
    m_attachBtn->setFixedSize(38, 38);
    m_attachBtn->setFlat(true);
    m_attachBtn->setToolTip("Attach file");
    m_attachBtn->setCursor(Qt::PointingHandCursor);
    m_attachBtn->setStyleSheet(
        "QPushButton { font-size: 18px; border: none; border-radius: 19px; background: transparent; }"
        "QPushButton:hover { background: #2c2c28; }"
    );
    layout->addWidget(m_attachBtn);

    m_input = new ComposeTextEdit(this);
    m_input->setPlaceholderText("Message...");
    m_input->setMinimumHeight(m_minInputH);
    m_input->setMaximumHeight(m_maxInputH);
    setFocusProxy(m_input);
    m_input->setStyleSheet(
        "QTextEdit { background: #222220; border: 1px solid #2a2a26; border-radius: 19px;"
        "  padding: 6px 14px; font-size: 14px; color: #e4e0da; }"
        "QTextEdit:focus { border-color: #2ec4b6; }"
    );
    layout->addWidget(m_input, 1);

    m_emojiBtn = new QPushButton(QStringLiteral("\U0001F600"), this);
    m_emojiBtn->setFlat(true);
    m_emojiBtn->setFixedSize(32, 32);
    m_emojiBtn->setCursor(Qt::PointingHandCursor);
    m_emojiBtn->setStyleSheet(
        "QPushButton { background: transparent; font-size: 18px; border: none; }"
        "QPushButton:hover { background: rgba(255,255,255,0.08); border-radius: 6px; }"
    );
    layout->addWidget(m_emojiBtn);
    connect(m_emojiBtn, &QPushButton::clicked, this, &ComposerWidget::openEmojiPicker);

    m_sendBtn = new QPushButton(this);
    m_sendBtn->setText(QString::fromUtf8("\xE2\x9E\xA4"));  // arrow
    m_sendBtn->setFixedSize(38, 38);
    m_sendBtn->setToolTip("Send message (Enter)");
    m_sendBtn->setCursor(Qt::PointingHandCursor);
    m_sendBtn->setStyleSheet(
        "QPushButton { font-size: 18px; border: none; border-radius: 19px; background: #2ec4b6; color: white; }"
        "QPushButton:hover { background: #3dd4c6; }"
        "QPushButton:pressed { background: #25a99d; }"
    );
    layout->addWidget(m_sendBtn);

    connect(m_sendBtn, &QPushButton::clicked, this, &ComposerWidget::sendAction);

    m_sendBtn->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sendBtn, &QPushButton::customContextMenuRequested, this, [this](const QPoint &) {
        QMenu menu(this);
        QFont boldF = menu.font(); boldF.setBold(true);
        QAction *sendAct = menu.addAction(tr("Send"));
        sendAct->setFont(boldF);
        QAction *silentAct = menu.addAction(QStringLiteral("\U0001F515  ")
            + tr("Send silently") + QStringLiteral(" \u2014 ") + tr("no notification"));
        QAction *picked = menu.exec(QCursor::pos());
        if (picked == silentAct) {
            m_nextSendSilent = true;
            sendAction();
        } else if (picked == sendAct) {
            sendAction();
        }
    });

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
    connect(m_attachBtn, &QPushButton::clicked, this, [this]() {
        QMenu menu(this);
        QAction *fromDevice = menu.addAction(
            QStringLiteral("\U0001F4BB  ") + tr("From this device\u2026"));
        QAction *fromNc = menu.addAction(
            QStringLiteral("\u2601\uFE0F  ") + tr("From Nextcloud\u2026"));
        const bool canShareFromNc = m_model && m_model->api() && m_model->api()->isAuthenticated()
                                    && !m_model->conversationToken().isEmpty();
        fromNc->setEnabled(canShareFromNc);

        QPoint pos = m_attachBtn->mapToGlobal(QPoint(0, -menu.sizeHint().height()));
        QAction *picked = menu.exec(pos);
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
    m_pendingPreview->setFixedSize(48, 48);
    m_pendingPreview->setAlignment(Qt::AlignCenter);
    m_pendingPreview->setStyleSheet("background: #222220; border-radius: 6px;");
    pendingLayout->addWidget(m_pendingPreview);

    m_pendingName = new QLabel(m_pendingBar);
    m_pendingName->setStyleSheet("font-size: 12px; color: #b0aca5;");
    pendingLayout->addWidget(m_pendingName, 1);

    m_pendingCancelBtn = new QPushButton("\u2715", m_pendingBar);
    m_pendingCancelBtn->setFixedSize(32, 32);
    m_pendingCancelBtn->setStyleSheet("font-size: 14px; border: none; border-radius: 16px; background: #e06060; color: white;");
    m_pendingCancelBtn->setCursor(Qt::PointingHandCursor);
    m_pendingCancelBtn->setToolTip("Cancel");
    connect(m_pendingCancelBtn, &QPushButton::clicked, this, &ComposerWidget::cancelPendingFile);
    pendingLayout->addWidget(m_pendingCancelBtn);

    // Reply bar (hidden by default)
    m_replyBar = new QWidget(this);
    m_replyBar->hide();
    m_replyBar->setStyleSheet("background: #1e2a28; border-top: 1px solid #2a2a26;");
    // Teal accent bar on the left
    auto *accentBar = new QWidget(m_replyBar);
    accentBar->setFixedWidth(3);
    accentBar->setStyleSheet("background: #2ec4b6;");
    m_replyBar->setFixedHeight(36);
    auto *replyBarLayout = new QHBoxLayout(m_replyBar);
    replyBarLayout->setContentsMargins(0, 0, 8, 0);
    replyBarLayout->setSpacing(8);
    replyBarLayout->addWidget(accentBar);
    m_replyLabel = new QLabel(m_replyBar);
    m_replyLabel->setStyleSheet("font-size: 13px; color: #b0aca5;");
    replyBarLayout->addWidget(m_replyLabel, 1);
    auto *replyCancelBtn = new QPushButton("\u2715", m_replyBar);
    replyCancelBtn->setFixedSize(28, 28);
    replyCancelBtn->setFlat(true);
    replyCancelBtn->setCursor(Qt::PointingHandCursor);
    replyCancelBtn->setStyleSheet(
        "QPushButton { font-size: 14px; border: none; border-radius: 14px; color: #8a8680; }"
        "QPushButton:hover { background: rgba(255,255,255,0.1); color: #e4e0da; }"
    );
    connect(replyCancelBtn, &QPushButton::clicked, this, &ComposerWidget::hideReplyBar);
    replyBarLayout->addWidget(replyCancelBtn);

    // Editing bar (hidden by default)
    m_editingBar = new QWidget(this);
    m_editingBar->hide();
    m_editingBar->setStyleSheet("background: #2a241e; border-top: 1px solid #2a2a26;");
    auto *editAccent = new QWidget(m_editingBar);
    editAccent->setFixedWidth(3);
    editAccent->setStyleSheet("background: #e0a040;"); // warm amber (vs reply's teal)
    m_editingBar->setFixedHeight(36);
    auto *editBarLayout = new QHBoxLayout(m_editingBar);
    editBarLayout->setContentsMargins(0, 0, 8, 0);
    editBarLayout->setSpacing(8);
    editBarLayout->addWidget(editAccent);

    auto *editIcon = new QLabel(QStringLiteral("\u270F\uFE0F"), m_editingBar); // ✏️
    editIcon->setStyleSheet("font-size: 14px;");
    editBarLayout->addWidget(editIcon);

    m_editingPreview = new QLabel(m_editingBar);
    m_editingPreview->setStyleSheet("font-size: 13px; color: #b0aca5;");
    editBarLayout->addWidget(m_editingPreview, 1);

    auto *editCancelBtn = new QPushButton("\u2715", m_editingBar);
    editCancelBtn->setFixedSize(28, 28);
    editCancelBtn->setFlat(true);
    editCancelBtn->setCursor(Qt::PointingHandCursor);
    editCancelBtn->setStyleSheet(
        "QPushButton { font-size: 14px; border: none; border-radius: 14px; color: #8a8680; }"
        "QPushButton:hover { background: rgba(255,255,255,0.1); color: #e4e0da; }"
    );
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

    setMaximumHeight(280);
}

// Reserve vertical space above the input row for any combination of the
// pending-file / reply / editing bars (each is 36-56px; total ≤120).
static constexpr int kComposerBarsReserve = 120;

void ComposerWidget::autoResizeInput()
{
    if (!m_input) return;
    const int docH = int(std::ceil(m_input->document()->size().height()));
    const int frame = m_input->frameWidth() * 2;
    const int newH = qBound(m_minInputH, docH + frame + 4, m_maxInputH);
    if (m_input->height() != newH)
        m_input->setFixedHeight(newH);
}

void ComposerWidget::setTopicName(const QString &name)
{
    m_topicName = name;
    if (name.isEmpty())
        m_input->setPlaceholderText("Message...");
    else
        m_input->setPlaceholderText("Reply in " + name + "...");
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
    const int lineH = QFontMetrics(font).height();
    m_minInputH = lineH + 16;
    m_maxInputH = m_minInputH * 5 + 12;
    m_input->setMinimumHeight(m_minInputH);
    m_input->setMaximumHeight(m_maxInputH);
    m_sendBtn->setFixedSize(m_minInputH, m_minInputH);
    m_attachBtn->setFixedSize(m_minInputH, m_minInputH);
    int btnFontSize = qMax(12, font.pixelSize());
    m_sendBtn->setStyleSheet(QString("font-size: %1px; border: none; border-radius: %2px; background: #2ec4b6; color: white;").arg(btnFontSize).arg(m_minInputH / 2));
    m_attachBtn->setStyleSheet(QString("font-size: %1px; border: none; border-radius: %2px;").arg(btnFontSize).arg(m_minInputH / 2));
    setMaximumHeight(m_maxInputH + kComposerBarsReserve);
    autoResizeInput();
}

void ComposerWidget::showPendingFile(const QString &path)
{
    m_pendingFilePath = path;
    QFileInfo fi(path);
    m_pendingName->setText(fi.fileName());

    // Show thumbnail for images
    QImage img(path);
    if (!img.isNull()) {
        QPixmap pix = QPixmap::fromImage(img.scaled(48, 48, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        m_pendingPreview->setPixmap(pix);
    } else {
        m_pendingPreview->setText("\U0001F4C4");
        m_pendingPreview->setStyleSheet("background: #222220; border-radius: 6px; font-size: 24px;");
    }

    m_pendingBar->show();
    m_input->setPlaceholderText("Add a caption...");
    m_input->setFocus();
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
        ? QStringLiteral("Message...")
        : QStringLiteral("Reply in %1...").arg(m_topicName));
    m_input->setFocus();
}

void ComposerWidget::showReplyBar(const QString &author, const QString &preview)
{
    m_replyLabel->setText(QStringLiteral("<span style='color:#2ec4b6; font-weight:600;'>%1</span>  %2")
        .arg(author.toHtmlEscaped(), preview.toHtmlEscaped()));
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

void ComposerWidget::sendAction()
{
    // If a file is pending, Enter sends the file (with caption from composer)
    if (!m_pendingFilePath.isEmpty()) {
        confirmSendFile();
        return;
    }
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
        QString original = m_sendBtn->text();
        m_sendBtn->setText(QStringLiteral("\U0001F515"));
        QTimer::singleShot(500, this, [this, original]() {
            if (m_sendBtn) m_sendBtn->setText(original);
        });
    }

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
        m_completion = new QListWidget(this->window());
        m_completion->setWindowFlags(Qt::Popup);
        m_completion->setStyleSheet(kPopupStyle);
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
        [this, query, token](const QVector<MentionCandidate> &candidates) {
            if (m_mentionWordStart < 0) return;
            if (m_pendingMentionQuery != query) return;
            // Guard against stale response after conversation switch.
            if (!m_model || m_model->conversationToken() != token) return;

            if (candidates.isEmpty()) {
                if (m_mentionPopup) m_mentionPopup->hide();
                return;
            }

            if (!m_mentionPopup) {
                m_mentionPopup = new QListWidget(this->window());
                m_mentionPopup->setWindowFlags(Qt::Popup);
                m_mentionPopup->setStyleSheet(kPopupStyle);
                m_mentionPopup->setIconSize(QSize(24, 24));
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

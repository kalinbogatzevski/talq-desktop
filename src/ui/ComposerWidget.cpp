#include "ComposerWidget.h"
#include "core/SignalingClient.h"
#include "models/MessageListModel.h"
#include <QFileDialog>
#include <QKeyEvent>
#include <QMimeData>
#include <QClipboard>
#include <QApplication>
#include <QImage>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>

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
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) {
            if (e->modifiers() & Qt::ShiftModifier) {
                QTextEdit::keyPressEvent(e);
            } else {
                e->accept();
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
                img.save(path, "PNG");
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
    m_input->setMaximumHeight(120);
    m_input->setMinimumHeight(38);
    setFocusProxy(m_input);
    m_input->setStyleSheet(
        "QTextEdit { background: #222220; border: 1px solid #2a2a26; border-radius: 19px;"
        "  padding: 6px 14px; font-size: 14px; color: #e4e0da; }"
        "QTextEdit:focus { border-color: #2ec4b6; }"
    );
    layout->addWidget(m_input, 1);

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
    connect(m_attachBtn, &QPushButton::clicked, this, [this]() {
        QString file = QFileDialog::getOpenFileName(this, "Send file");
        if (!file.isEmpty())
            showPendingFile(file);
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

    // Insert bars above the input row
    auto *mainLayout = new QVBoxLayout();
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    mainLayout->addWidget(m_pendingBar);
    mainLayout->addWidget(m_replyBar);

    auto *inputRow = new QWidget(this);
    inputRow->setLayout(layout);
    mainLayout->addWidget(inputRow);
    setLayout(mainLayout);

    setMaximumHeight(200);
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
}

void ComposerWidget::setInputFont(const QFont &font)
{
    m_input->setFont(font);
    // Scale the input field height to match the font
    int lineH = QFontMetrics(font).height();
    int fieldH = lineH + 16;
    m_input->setFixedHeight(fieldH);
    m_sendBtn->setFixedSize(fieldH, fieldH);
    m_attachBtn->setFixedSize(fieldH, fieldH);
    int btnFontSize = qMax(12, font.pixelSize());
    m_sendBtn->setStyleSheet(QString("font-size: %1px; border: none; border-radius: %2px; background: #2ec4b6; color: white;").arg(btnFontSize).arg(fieldH / 2));
    m_attachBtn->setStyleSheet(QString("font-size: %1px; border: none; border-radius: %2px;").arg(btnFontSize).arg(fieldH / 2));
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
    if (m_pendingFilePath.isEmpty() || !m_model) return;
    QString caption = m_input->toPlainText().trimmed();
    m_model->sendFileWithCaption(QUrl::fromLocalFile(m_pendingFilePath).toString(), caption);
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
    emit sendMessage(text);
    m_input->clear();
}

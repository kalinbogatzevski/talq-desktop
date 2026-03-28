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
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(8, 4, 8, 4);
    layout->setSpacing(6);

    m_attachBtn = new QPushButton(this);
    m_attachBtn->setText(QString::fromUtf8("\xF0\x9F\x93\x8E"));  // clip
    m_attachBtn->setFixedSize(32, 32);
    m_attachBtn->setFlat(true);
    m_attachBtn->setToolTip("Attach file");
    layout->addWidget(m_attachBtn);

    m_input = new ComposeTextEdit(this);
    m_input->setPlaceholderText("Message...");
    m_input->setMaximumHeight(120);
    m_input->setMinimumHeight(36);
    layout->addWidget(m_input, 1);

    m_sendBtn = new QPushButton(this);
    m_sendBtn->setText(QString::fromUtf8("\xE2\x9E\xA4"));  // arrow
    m_sendBtn->setFixedSize(36, 36);
    m_sendBtn->setToolTip("Send message (Enter)");
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

    auto *infoCol = new QVBoxLayout();
    infoCol->setSpacing(2);
    m_pendingName = new QLabel(m_pendingBar);
    m_pendingName->setStyleSheet("font-size: 12px; color: #8a8680;");
    infoCol->addWidget(m_pendingName);

    m_captionInput = new QLineEdit(m_pendingBar);
    m_captionInput->setPlaceholderText("Add a caption...");
    m_captionInput->setStyleSheet("font-size: 13px; padding: 4px 8px; border-radius: 4px;");
    connect(m_captionInput, &QLineEdit::returnPressed, this, &ComposerWidget::confirmSendFile);
    infoCol->addWidget(m_captionInput);
    pendingLayout->addLayout(infoCol, 1);

    m_pendingSendBtn = new QPushButton("\u2713", m_pendingBar);
    m_pendingSendBtn->setFixedSize(32, 32);
    m_pendingSendBtn->setStyleSheet("font-size: 16px; border: none; border-radius: 16px; background: #2ec4b6; color: white;");
    m_pendingSendBtn->setCursor(Qt::PointingHandCursor);
    m_pendingSendBtn->setToolTip("Send");
    connect(m_pendingSendBtn, &QPushButton::clicked, this, &ComposerWidget::confirmSendFile);
    pendingLayout->addWidget(m_pendingSendBtn);

    m_pendingCancelBtn = new QPushButton("\u2715", m_pendingBar);
    m_pendingCancelBtn->setFixedSize(32, 32);
    m_pendingCancelBtn->setStyleSheet("font-size: 14px; border: none; border-radius: 16px; background: #e06060; color: white;");
    m_pendingCancelBtn->setCursor(Qt::PointingHandCursor);
    m_pendingCancelBtn->setToolTip("Cancel");
    connect(m_pendingCancelBtn, &QPushButton::clicked, this, &ComposerWidget::cancelPendingFile);
    pendingLayout->addWidget(m_pendingCancelBtn);

    // Insert pending bar above the input row
    auto *mainLayout = new QVBoxLayout();
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    mainLayout->addWidget(m_pendingBar);

    // Re-parent the existing layout into the main layout
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
    m_captionInput->clear();

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
    m_captionInput->setFocus();
}

void ComposerWidget::confirmSendFile()
{
    if (m_pendingFilePath.isEmpty() || !m_model) return;
    QString caption = m_captionInput->text().trimmed();
    m_model->sendFileWithCaption(QUrl::fromLocalFile(m_pendingFilePath).toString(), caption);
    cancelPendingFile();
}

void ComposerWidget::cancelPendingFile()
{
    m_pendingFilePath.clear();
    m_pendingBar->hide();
    m_captionInput->clear();
    m_input->setFocus();
}

void ComposerWidget::sendAction()
{
    QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty()) return;
    if (m_signaling) m_signaling->sendStoppedTyping();
    emit sendMessage(text);
    m_input->clear();
}

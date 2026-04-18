#include "ImageViewerDialog.h"

#include "core/ApiClient.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

ImageViewerDialog::ImageViewerDialog(ApiClient *api, QWidget *parent)
    : QWidget(parent, Qt::Window), m_api(api)
{
    setWindowTitle(tr("Image viewer"));
    setStyleSheet("QWidget { background: #000; color: #eee; }");

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *titleRow = new QWidget(this);
    titleRow->setStyleSheet("background: rgba(0,0,0,0.6);");
    auto *titleRowLayout = new QHBoxLayout(titleRow);
    titleRowLayout->setContentsMargins(12, 6, 6, 6);
    titleRowLayout->setSpacing(6);

    m_titleBar = new QLabel(titleRow);
    m_titleBar->setStyleSheet("font-size: 13px; background: transparent;");
    m_titleBar->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    titleRowLayout->addWidget(m_titleBar, 1);

    m_menuBtn = new QPushButton(QStringLiteral("\u22EF"), titleRow); // ⋯
    m_menuBtn->setFlat(true);
    m_menuBtn->setFixedSize(28, 24);
    m_menuBtn->setCursor(Qt::PointingHandCursor);
    m_menuBtn->setStyleSheet(
        "QPushButton { background: transparent; color: #eee; font-size: 16px; border: none; }"
        "QPushButton:hover { background: rgba(255,255,255,0.08); border-radius: 4px; }"
    );
    titleRowLayout->addWidget(m_menuBtn);

    root->addWidget(titleRow);

    m_scene = new QGraphicsScene(this);
    m_view  = new QGraphicsView(m_scene, this);
    m_view->setFrameShape(QFrame::NoFrame);
    m_view->setBackgroundBrush(Qt::black);
    m_view->setDragMode(QGraphicsView::ScrollHandDrag);
    m_view->setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    m_view->setRenderHint(QPainter::SmoothPixmapTransform);
    m_view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    root->addWidget(m_view, 1);

    m_view->viewport()->installEventFilter(this);

    connect(m_menuBtn, &QPushButton::clicked, this, [this]() {
        QMenu menu(this);
        menu.addAction(tr("Copy image"), QKeySequence(QKeySequence::Copy),
                       this, &ImageViewerDialog::copyImage);
        menu.addAction(tr("Save as\u2026"), QKeySequence(QKeySequence::Save),
                       this, &ImageViewerDialog::saveAs);
        QPoint p = m_menuBtn->mapToGlobal(QPoint(0, m_menuBtn->height()));
        menu.exec(p);
    });

    QSettings s;
    QRect g = s.value(QStringLiteral("imageViewer/geometry")).toRect();
    if (g.isValid()) setGeometry(g);
    else if (screen()) {
        QSize sz = screen()->availableSize() * 0.8;
        resize(sz);
    } else {
        resize(1000, 700);
    }
}

void ImageViewerDialog::applyPixmap(const QImage &img)
{
    m_currentImage = img;
    m_scene->clear();
    m_item = m_scene->addPixmap(QPixmap::fromImage(img));
    m_scene->setSceneRect(m_item->boundingRect());
    fitToWindow();
    m_at100 = false;
}

void ImageViewerDialog::fitToWindow()
{
    if (!m_item) return;
    m_view->resetTransform();
    m_view->fitInView(m_item, Qt::KeepAspectRatio);
}

void ImageViewerDialog::actualSize()
{
    if (!m_item) return;
    m_view->resetTransform();
}

void ImageViewerDialog::zoomByStep(bool zoomIn)
{
    const qreal s = zoomIn ? 1.15 : (1.0 / 1.15);
    m_view->scale(s, s);
}

void ImageViewerDialog::setImage(int fileId, const QString &fileName, const QImage &placeholder)
{
    m_currentFileId = fileId;
    m_currentFileName = fileName;
    m_titleBar->setText(m_currentFileName);
    if (!placeholder.isNull()) applyPixmap(placeholder);

    if (m_api) {
        m_api->fetchFileImage(fileId, this,
            [this, fileId](const QImage &img, const QString &err) {
            if (fileId != m_currentFileId) return; // user navigated away
            if (!img.isNull()) applyPixmap(img);
            else if (!err.isEmpty())
                m_titleBar->setText(m_currentFileName + QStringLiteral("  —  ") + err);
        });
    }
}

void ImageViewerDialog::copyImage()
{
    if (m_currentImage.isNull()) return;
    QApplication::clipboard()->setImage(m_currentImage);

    // Brief visual confirmation — restore from m_currentFileName (source of
    // truth) rather than whatever suffix the title bar happens to show.
    m_titleBar->setText(m_currentFileName + QStringLiteral("  —  copied to clipboard"));
    QTimer::singleShot(2000, this, [this]() {
        if (!m_titleBar) return;
        m_titleBar->setText(m_currentFileName);
    });
}

void ImageViewerDialog::saveAs()
{
    if (m_currentImage.isNull()) return;

    QString defaultName = m_currentFileName.isEmpty()
        ? QStringLiteral("image.png") : m_currentFileName;
    if (QFileInfo(defaultName).suffix().isEmpty())
        defaultName += QStringLiteral(".png");

    QString path = QFileDialog::getSaveFileName(
        this, tr("Save image"), defaultName,
        tr("Images (*.png *.jpg *.jpeg *.bmp)"));
    if (path.isEmpty()) return;

    if (!m_currentImage.save(path)) {
        QMessageBox::warning(this, tr("Save failed"),
            tr("Could not save image to:\n%1").arg(path));
    }
}

void ImageViewerDialog::keyPressEvent(QKeyEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        switch (event->key()) {
        case Qt::Key_C: copyImage(); return;
        case Qt::Key_S: saveAs();    return;
        default: break;
        }
    }
    switch (event->key()) {
    case Qt::Key_Escape: close(); return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:  zoomByStep(true);  return;
    case Qt::Key_Minus:  zoomByStep(false); return;
    case Qt::Key_0:      fitToWindow(); m_at100 = false; return;
    case Qt::Key_1:      actualSize(); m_at100 = true;   return;
    }
    QWidget::keyPressEvent(event);
}

void ImageViewerDialog::mouseMoveEvent(QMouseEvent *event)
{
    m_titleBar->show();
    QWidget::mouseMoveEvent(event);
}

void ImageViewerDialog::closeEvent(QCloseEvent *event)
{
    QSettings().setValue(QStringLiteral("imageViewer/geometry"), geometry());
    QWidget::closeEvent(event);
}

bool ImageViewerDialog::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view->viewport()) {
        if (event->type() == QEvent::Wheel) {
            auto *w = static_cast<QWheelEvent*>(event);
            if (w->modifiers() & Qt::ControlModifier) {
                zoomByStep(w->angleDelta().y() > 0);
                return true;
            }
        } else if (event->type() == QEvent::MouseButtonDblClick) {
            if (m_at100) { fitToWindow(); m_at100 = false; }
            else         { actualSize();  m_at100 = true;  }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

#include "ImageViewerDialog.h"

#include "core/ApiClient.h"

#include <QApplication>
#include <QClipboard>
#include <QCloseEvent>
#include <QContextMenuEvent>
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
#include <QResizeEvent>
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

    // Centered confirmation pill. Parented to the dialog so it floats above
    // the QGraphicsView regardless of where the user has scrolled/zoomed.
    m_toast = new QLabel(this);
    m_toast->setStyleSheet(
        "background: rgba(0,0,0,0.78);"
        "color: #ffffff;"
        "font-size: 14px;"
        "padding: 10px 18px;"
        "border-radius: 18px;"
    );
    m_toast->setAlignment(Qt::AlignCenter);
    m_toast->setAttribute(Qt::WA_TransparentForMouseEvents);  // never steals clicks
    m_toast->hide();

    m_toastHideTimer = new QTimer(this);
    m_toastHideTimer->setSingleShot(true);
    m_toastHideTimer->setInterval(1400);
    connect(m_toastHideTimer, &QTimer::timeout, this, [this]() {
        if (m_toast) m_toast->hide();
    });

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
    // Name the OS window (taskbar / alt-tab) after the file, not a generic
    // "Image viewer", and bring it to the front — clicking an image while
    // the viewer is already open (e.g. behind the call window) must surface
    // it, not leave it stranded under another window.
    setWindowTitle(fileName.isEmpty() ? tr("Image viewer") : fileName);
    raise();
    activateWindow();
    if (!placeholder.isNull()) applyPixmap(placeholder);

    if (m_api) {
        // Size the fetch to the available screen so we don't pull 4K for a
        // viewer that might only be 1080px wide.
        int maxDim = 2048;
        if (auto *s = screen())
            maxDim = qMax(s->availableSize().width(), s->availableSize().height());
        // Loading feedback: the full-res fetch can take a while on slow
        // links, during which only the upscaled thumbnail shows. Make that
        // explicit instead of looking like a "stuck on thumbnail" bug.
        m_titleBar->setText(m_currentFileName + QStringLiteral("   ·   ") + tr("Loading full image…"));
        m_api->fetchFileImage(fileId, maxDim, this,
            [this, fileId](const QImage &img, const QString &err) {
            if (fileId != m_currentFileId) return; // user navigated away
            if (!img.isNull()) {
                applyPixmap(img);
                m_titleBar->setText(m_currentFileName);
            } else {
                m_titleBar->setText(m_currentFileName + QStringLiteral("   ·   ")
                    + (err.isEmpty() ? tr("Couldn't load full image") : err));
            }
        });
    } else {
        m_titleBar->setText(m_currentFileName);
    }
}

void ImageViewerDialog::copyImage()
{
    if (m_currentImage.isNull()) return;
    QApplication::clipboard()->setImage(m_currentImage);
    showToast(tr("Copied to clipboard"));
}

void ImageViewerDialog::showToast(const QString &text)
{
    if (!m_toast) return;
    m_toast->setText(text);
    m_toast->adjustSize();
    positionToast();
    m_toast->show();
    m_toast->raise();
    if (m_toastHideTimer) m_toastHideTimer->start();
}

void ImageViewerDialog::positionToast()
{
    if (!m_toast) return;
    const QSize hint = m_toast->sizeHint();
    // Bottom-centered: feels native (matches OS-level toast positioning)
    // and stays out of the title-bar / menu region.
    const int x = (width() - hint.width()) / 2;
    const int y = height() - hint.height() - 40;
    m_toast->setGeometry(x, y, hint.width(), hint.height());
}

void ImageViewerDialog::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_toast && m_toast->isVisible()) positionToast();
}

void ImageViewerDialog::contextMenuEvent(QContextMenuEvent *event)
{
    // Right-click anywhere over the viewer pops the same menu as the ⋯
    // button — that's the discoverability path most users reach for first.
    QMenu menu(this);
    menu.addAction(tr("Copy image"), QKeySequence(QKeySequence::Copy),
                   this, &ImageViewerDialog::copyImage);
    menu.addAction(tr("Save as…"), QKeySequence(QKeySequence::Save),
                   this, &ImageViewerDialog::saveAs);
    menu.exec(event->globalPos());
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

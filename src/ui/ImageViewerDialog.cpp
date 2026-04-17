#include "ImageViewerDialog.h"

#include "core/ApiClient.h"

#include <QCloseEvent>
#include <QGraphicsPixmapItem>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QKeyEvent>
#include <QLabel>
#include <QScreen>
#include <QSettings>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QMouseEvent>

ImageViewerDialog::ImageViewerDialog(ApiClient *api, QWidget *parent)
    : QWidget(parent, Qt::Window), m_api(api)
{
    setWindowTitle(tr("Image viewer"));
    setStyleSheet("QWidget { background: #000; color: #eee; }");

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    m_titleBar = new QLabel(this);
    m_titleBar->setStyleSheet("background: rgba(0,0,0,0.6); padding: 6px 12px; font-size: 13px;");
    m_titleBar->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    root->addWidget(m_titleBar);

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
    m_titleBar->setText(fileName);
    if (!placeholder.isNull()) applyPixmap(placeholder);

    if (m_api) {
        m_api->fetchFileImage(fileId, [this, fileId, fileName](const QImage &img, const QString &err) {
            if (fileId != m_currentFileId) return; // user navigated away
            if (!img.isNull()) applyPixmap(img);
            else if (!err.isEmpty())
                m_titleBar->setText(fileName + QStringLiteral("  —  ") + err);
        });
    }
}

void ImageViewerDialog::keyPressEvent(QKeyEvent *event)
{
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

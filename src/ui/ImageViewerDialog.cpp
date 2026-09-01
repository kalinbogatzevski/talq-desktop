#include "ImageViewerDialog.h"

#include "core/ApiClient.h"
#include "painter/PainterTheme.h"

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

namespace {
// A media viewer keeps a deliberately dark backdrop (DESIGN.md sanctions a dark
// media surface), so chrome must stay legible on that dark scrim regardless of
// the app theme. We drive every text/scrim/hover colour from PainterTheme, but
// for the always-dark backdrop we resolve a warm-DARK theme: the user's active
// theme unless it's the light Paper one, in which case we fall back to the warm
// Vivid default so text doesn't render dark-on-dark.
PainterTheme viewerChromeTheme()
{
    QSettings s;
    s.beginGroup(QStringLiteral("Theme"));
    PainterTheme::Theme t = PainterTheme::themeFromId(
        s.value(QStringLiteral("theme"),
                PainterTheme::themeId(PainterTheme::Theme::Vivid)).toString(),
        PainterTheme::Theme::Vivid);
    s.endGroup();
    if (t == PainterTheme::Theme::Paper)
        t = PainterTheme::Theme::Vivid;
    return PainterTheme(t, 1.0);
}
} // namespace

ImageViewerDialog::ImageViewerDialog(ApiClient *api, QWidget *parent)
    : QWidget(parent, Qt::Window), m_api(api)
{
    setWindowTitle(tr("Image viewer"));

    const PainterTheme th = viewerChromeTheme();
    const QString ink = th.textPrimary.name(QColor::HexRgb);
    // Translucent "rgba(r,g,b,a)" from a theme colour — for warm scrims/hovers
    // that sit over the image, replacing the old #000/#fff-alpha literals.
    auto rgba = [](const QColor &c, double a) {
        return QStringLiteral("rgba(%1,%2,%3,%4)")
            .arg(c.red()).arg(c.green()).arg(c.blue()).arg(a, 0, 'g', 3);
    };
    // Dark media backdrop is intentional; use the darkest warm ground token
    // (never #000) so the window chrome stays on-theme and warm, not cold black.
    const QString backdrop = th.bgSidebar.name(QColor::HexRgb);
    setStyleSheet(QStringLiteral("QWidget { background: %1; color: %2; }")
                      .arg(backdrop, ink));

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *titleRow = new QWidget(this);
    titleRow->setStyleSheet(QStringLiteral("background: %1;").arg(rgba(th.bgSidebar, 0.6)));
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
    m_menuBtn->setStyleSheet(QStringLiteral(
        "QPushButton { background: transparent; color: %1; font-size: 16px; border: none; }"
        "QPushButton:hover { background: %2; border-radius: 4px; }"
    ).arg(ink, rgba(th.textPrimary, 0.08)));
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
    m_toast->setStyleSheet(QStringLiteral(
        "background: %1;"
        "color: %2;"
        "font-size: 14px;"
        "padding: 10px 18px;"
        "border-radius: 18px;"
    ).arg(rgba(th.bgSidebar, 0.78), ink));
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

    // The filter follows the file's OWN type. It used to be a fixed
    // "Images (*.png *.jpg *.jpeg *.bmp)", which quietly proposed saving a
    // .gif, .webp, .heic or .svg under an extension it is not -- and since
    // QImage::save picks its writer from the extension, agreeing to that
    // silently transcoded the picture.
    const QString suffix = QFileInfo(defaultName).suffix().toLower();
    const QString filter = suffix.isEmpty()
        ? tr("All files (*)")
        : tr("%1 file (*.%2);;All files (*)").arg(suffix.toUpper(), suffix);

    QString path = QFileDialog::getSaveFileName(
        this, tr("Save image"), defaultName, filter);
    if (path.isEmpty()) return;

    // Ask for the ORIGINAL bytes when we know which file this is. What is on
    // screen is a server-side preview render capped to the screen's largest
    // edge (see setImage -> fetchFileImage), so saving it wrote a downscaled
    // re-encode of a photo the user believed they were saving intact.
    if (m_currentFileId > 0) {
        emit saveOriginalRequested(m_currentFileId, m_currentFileName, path);
        return;
    }

    // No fileId: nothing to fetch, so the on-screen render is genuinely all
    // there is. Save it rather than refusing.
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
    // Reveal the title bar on movement, then auto-hide it after a short idle so
    // it doesn't sit over the image. The hide timer is created lazily and kept
    // as a named child (no extra header member) and restarted on every move.
    m_titleBar->show();
    auto *hideTimer = findChild<QTimer *>(QStringLiteral("titleBarHideTimer"),
                                          Qt::FindDirectChildrenOnly);
    if (!hideTimer) {
        hideTimer = new QTimer(this);
        hideTimer->setObjectName(QStringLiteral("titleBarHideTimer"));
        hideTimer->setSingleShot(true);
        hideTimer->setInterval(2000);
        connect(hideTimer, &QTimer::timeout, this, [this]() {
            if (m_titleBar) m_titleBar->hide();
        });
    }
    hideTimer->start();
    QWidget::mouseMoveEvent(event);
}

void ImageViewerDialog::closeEvent(QCloseEvent *event)
{
    QSettings().setValue(QStringLiteral("imageViewer/geometry"), geometry());
    // Closing only hid the dialog, so the last full-resolution image (capped
    // to the largest screen edge — tens of MB on a 4K display) and the scene
    // pixmap stayed resident until the next image replaced them.
    m_scene->clear();
    m_item = nullptr;
    m_currentImage = QImage();
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

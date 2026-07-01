#include "SharePickerDialog.h"
#include <QScreen>
#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QIcon>
#include <QComboBox>
#include <QHBoxLayout>
#include <QSettings>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
// PW_RENDERFULLCONTENT (Win 8.1+) captures DWM-composited / GPU-accelerated
// windows that a plain PrintWindow renders blank. Older SDK headers omit it.
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif
#endif

namespace {

constexpr int kThumbW = 192;
constexpr int kThumbH = 108;   // 16:9 preview

// One placeholder so a not-yet-captured / uncapturable target still reads
// as a tile, never an empty box.
QPixmap placeholderThumb()
{
    QPixmap pm(kThumbW, kThumbH);
    pm.fill(QApplication::palette().color(QPalette::Mid));
    return pm;
}

#ifdef Q_OS_WIN
// Live capture of a top-level window into a thumbnail. Returns a null
// pixmap when the window can't be grabbed (minimized, protected, etc.) so
// the caller can fall back to the window's program icon.
QPixmap grabWindowThumb(HWND hwnd)
{
    if (!IsWindow(hwnd) || IsIconic(hwnd)) return {};
    RECT r;
    if (!GetWindowRect(hwnd, &r)) return {};
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384) return {};

    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bmp = CreateCompatibleBitmap(screenDC, w, h);
    HGDIOBJ oldObj = SelectObject(memDC, bmp);

    QImage img;
    if (PrintWindow(hwnd, memDC, PW_RENDERFULLCONTENT)) {
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;          // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        QImage tmp(w, h, QImage::Format_RGB32);
        if (GetDIBits(memDC, bmp, 0, h, tmp.bits(), &bi, DIB_RGB_COLORS))
            img = tmp.copy();
    }

    SelectObject(memDC, oldObj);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);

    if (img.isNull()) return {};
    return QPixmap::fromImage(img).scaled(kThumbW, kThumbH,
        Qt::KeepAspectRatio, Qt::SmoothTransformation);
}

// Program icon fallback when the window itself can't be captured.
QPixmap windowProgramIcon(HWND hwnd)
{
    HICON hIcon = (HICON)SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0);
    if (!hIcon) hIcon = (HICON)GetClassLongPtrW(hwnd, GCLP_HICON);
    if (!hIcon) hIcon = (HICON)SendMessageW(hwnd, WM_GETICON, ICON_SMALL, 0);
    if (!hIcon) return {};
    QImage img = QImage::fromHICON(hIcon);
    if (img.isNull()) return {};
    return QPixmap::fromImage(img);
}
#endif

// When a populate pass finds nothing, drop in a single non-selectable
// placeholder so the tab reads as "empty", not "broken" (a blank grid with a
// silently-disabled Share button). Non-selectable → it can never enable Share.
void addEmptyPlaceholder(QListWidget *lw, const QString &text)
{
    if (lw->count() > 0) return;
    auto *item = new QListWidgetItem(text, lw);
    item->setFlags(Qt::NoItemFlags);
    item->setTextAlignment(Qt::AlignCenter);
}

} // namespace

SharePickerDialog::SharePickerDialog(QWidget *parent)
    : QDialog(parent, Qt::Dialog)
{
    setWindowTitle(tr("Share Screen"));
    setMinimumSize(560, 440);
    resize(640, 500);
    // Chrome inherited from the app-wide AppStyle sheet (theme-driven).

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(12);

    auto *title = new QLabel(tr("Choose what to share"), this);
    title->setProperty("role", "title");
    layout->addWidget(title);

    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs, 1);

    auto styleList = [](QListWidget *lw) {
        lw->setViewMode(QListView::IconMode);
        lw->setIconSize(QSize(kThumbW, kThumbH));
        // 0.53.0 — give the title generous room UNDER the thumbnail so the full
        // window/app title is ALWAYS visible, not truncated-to-a-hover-tooltip:
        // ~4 wrapped lines (kThumbH + 76). If a title is still too long, elide in
        // the MIDDLE so the trailing app name (e.g. "… — TalQ") stays readable.
        lw->setGridSize(QSize(kThumbW + 28, kThumbH + 76));
        lw->setResizeMode(QListView::Adjust);
        lw->setMovement(QListView::Static);
        lw->setSelectionMode(QAbstractItemView::SingleSelection);
        lw->setUniformItemSizes(true);
        lw->setWordWrap(true);
        lw->setTextElideMode(Qt::ElideMiddle);
        lw->setSpacing(10);
    };

    m_monitorList = new QListWidget(this);
    styleList(m_monitorList);
    m_tabs->addTab(m_monitorList, tr("Screens"));

    m_windowList = new QListWidget(this);
    styleList(m_windowList);
    m_tabs->addTab(m_windowList, tr("Windows"));

    // Quality (pre-share). Persisted to Video/screenShareQuality, which
    // CallManager::startScreenShare reads; it can also be changed live
    // mid-share via CallManager::setScreenShareQuality (#120).
    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(0, 8, 0, 0);
    bottom->setSpacing(10);
    auto *qLbl = new QLabel(tr("Quality"), this);
    qLbl->setProperty("role", "secondary");
    auto *qCombo = new QComboBox(this);
    qCombo->addItem(tr("720p"), 0);
    qCombo->addItem(tr("1080p"), 1);
    qCombo->addItem(tr("1440p"), 2);
    qCombo->addItem(tr("Native"), 3);
    {
        int cur = QSettings("TalQ", "TalQ")
                      .value("Video/screenShareQuality", 1).toInt();
        int idx = qCombo->findData(cur);
        qCombo->setCurrentIndex(idx < 0 ? 1 : idx);
    }
    connect(qCombo, QOverload<int>::of(&QComboBox::activated), this,
            [qCombo](int) {
        QSettings("TalQ", "TalQ").setValue(
            "Video/screenShareQuality", qCombo->currentData().toInt());
    });
    bottom->addWidget(qLbl);
    bottom->addWidget(qCombo);
    bottom->addStretch();

    m_shareBtn = new QPushButton(tr("Share"), this);
    m_shareBtn->setProperty("variant", "primary");
    m_shareBtn->setEnabled(false);
    bottom->addWidget(m_shareBtn);
    layout->addLayout(bottom);

    connect(m_monitorList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row < m_monitors.size()) {
            m_selected = m_monitors[row];
            m_shareBtn->setEnabled(true);
            m_windowList->clearSelection();
        }
    });
    connect(m_windowList, &QListWidget::currentRowChanged, this, [this](int row) {
        if (row >= 0 && row < m_windows.size()) {
            m_selected = m_windows[row];
            m_shareBtn->setEnabled(true);
            m_monitorList->clearSelection();
        }
    });
    connect(m_monitorList, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
    connect(m_windowList, &QListWidget::itemDoubleClicked, this, &QDialog::accept);
    connect(m_shareBtn, &QPushButton::clicked, this, &QDialog::accept);

    populateMonitors();
    populateWindows();
    refreshThumbnails();

    // Live preview: refresh thumbnails while the picker is open so a
    // window/screen the user is about to share is shown current, not
    // stale. Cheap (a handful of scaled grabs) and only while visible —
    // the timer dies with the dialog.
    m_thumbTimer = new QTimer(this);
    m_thumbTimer->setInterval(1500);
    connect(m_thumbTimer, &QTimer::timeout, this, &SharePickerDialog::refreshThumbnails);
    m_thumbTimer->start();
}

void SharePickerDialog::populateMonitors()
{
    m_monitors.clear();
    m_monitorList->clear();
    const auto screens = QApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        ShareTarget t;
        t.type = ShareTarget::Monitor;
        t.monitorIndex = i;
        QRect geo = screens[i]->geometry();
        t.name = QString("Screen %1  (%2 × %3)")
                     .arg(i + 1).arg(geo.width()).arg(geo.height());
        if (screens[i] == QApplication::primaryScreen())
            t.name += "  · Primary";
        m_monitors.append(t);

        auto *item = new QListWidgetItem(QIcon(placeholderThumb()),
                                         t.name, m_monitorList);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
    }
    addEmptyPlaceholder(m_monitorList, tr("No shareable screens found"));
}

void SharePickerDialog::populateWindows()
{
    m_windows.clear();
    m_windowList->clear();
#ifdef Q_OS_WIN
    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto *self = reinterpret_cast<SharePickerDialog *>(lParam);

        if (!IsWindowVisible(hwnd)) return TRUE;
        if (IsIconic(hwnd)) return TRUE;

        wchar_t title[256];
        int len = GetWindowTextW(hwnd, title, 256);
        if (len == 0) return TRUE;

        QString name = QString::fromWCharArray(title, len);
        if (name == "Program Manager" || name == "Windows Input Experience"
            || name == "Microsoft Text Input Application")
            return TRUE;

        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;

        ShareTarget t;
        t.type = ShareTarget::Window;
        t.windowHandle = reinterpret_cast<quintptr>(hwnd);
        t.name = name;
        self->m_windows.append(t);

        auto *item = new QListWidgetItem(QIcon(placeholderThumb()),
                                         name, self->m_windowList);
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignTop);
        item->setToolTip(name);
        return TRUE;
    }, reinterpret_cast<LPARAM>(this));
#endif
    addEmptyPlaceholder(m_windowList, tr("No shareable windows found"));
}

void SharePickerDialog::refreshThumbnails()
{
    const auto screens = QApplication::screens();
    for (int i = 0; i < m_monitors.size() && i < m_monitorList->count(); ++i) {
        if (i >= screens.size()) continue;
        QPixmap shot = screens[i]->grabWindow(0);
        if (!shot.isNull())
            m_monitorList->item(i)->setIcon(QIcon(shot.scaled(
                kThumbW, kThumbH, Qt::KeepAspectRatio,
                Qt::SmoothTransformation)));
    }

#ifdef Q_OS_WIN
    for (int i = 0; i < m_windows.size() && i < m_windowList->count(); ++i) {
        HWND hwnd = reinterpret_cast<HWND>(m_windows[i].windowHandle);
        QPixmap thumb = grabWindowThumb(hwnd);
        if (thumb.isNull()) {
            QPixmap ic = windowProgramIcon(hwnd);
            thumb = ic.isNull() ? placeholderThumb()
                                : ic.scaled(64, 64, Qt::KeepAspectRatio,
                                            Qt::SmoothTransformation);
        }
        m_windowList->item(i)->setIcon(QIcon(thumb));
    }
#endif
}

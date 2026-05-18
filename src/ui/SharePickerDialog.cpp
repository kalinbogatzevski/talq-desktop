#include "SharePickerDialog.h"
#include <QScreen>
#include <QApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

SharePickerDialog::SharePickerDialog(QWidget *parent)
    : QDialog(parent, Qt::Dialog)
{
    setWindowTitle("Share Screen");
    setMinimumSize(400, 350);
    resize(450, 400);
    // Chrome inherited from the app-wide AppStyle sheet (theme-driven).

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);

    auto *title = new QLabel("Choose what to share", this);
    title->setProperty("role", "title");
    layout->addWidget(title);

    m_tabs = new QTabWidget(this);
    layout->addWidget(m_tabs, 1);

    // Screens tab
    m_monitorList = new QListWidget(this);
    m_tabs->addTab(m_monitorList, "Screens");

    // Windows tab
    m_windowList = new QListWidget(this);
    m_tabs->addTab(m_windowList, "Windows");

    // Share button
    m_shareBtn = new QPushButton("Share", this);
    m_shareBtn->setProperty("variant", "primary");
    m_shareBtn->setEnabled(false);
    layout->addWidget(m_shareBtn, 0, Qt::AlignRight);

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
}

void SharePickerDialog::populateMonitors()
{
    m_monitors.clear();
    const auto screens = QApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        ShareTarget t;
        t.type = ShareTarget::Monitor;
        t.monitorIndex = i;
        QRect geo = screens[i]->geometry();
        t.name = QString("Screen %1 (%2x%3)").arg(i + 1).arg(geo.width()).arg(geo.height());
        if (screens[i] == QApplication::primaryScreen())
            t.name += " - Primary";
        m_monitors.append(t);

        auto *item = new QListWidgetItem(
            QString("\xF0\x9F\x96\xA5  %1").arg(t.name), m_monitorList);
        item->setSizeHint(QSize(0, 40));
    }
}

void SharePickerDialog::populateWindows()
{
#ifdef Q_OS_WIN
    m_windows.clear();

    EnumWindows([](HWND hwnd, LPARAM lParam) -> BOOL {
        auto *self = reinterpret_cast<SharePickerDialog *>(lParam);

        // Skip invisible, minimized, and tool windows
        if (!IsWindowVisible(hwnd)) return TRUE;
        if (IsIconic(hwnd)) return TRUE;

        // Skip windows with no title
        wchar_t title[256];
        int len = GetWindowTextW(hwnd, title, 256);
        if (len == 0) return TRUE;

        // Skip certain system windows
        QString name = QString::fromWCharArray(title, len);
        if (name == "Program Manager" || name == "Windows Input Experience"
            || name == "Microsoft Text Input Application")
            return TRUE;

        // Skip cloaked (invisible UWP) windows
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) return TRUE;

        // Get process name for context
        DWORD pid;
        GetWindowThreadProcessId(hwnd, &pid);

        ShareTarget t;
        t.type = ShareTarget::Window;
        t.windowHandle = reinterpret_cast<quintptr>(hwnd);
        t.name = name;
        self->m_windows.append(t);

        auto *item = new QListWidgetItem(
            QString("\xF0\x9F\x93\x8B  %1").arg(name), self->m_windowList);
        item->setSizeHint(QSize(0, 40));

        return TRUE;
    }, reinterpret_cast<LPARAM>(this));
#endif
}

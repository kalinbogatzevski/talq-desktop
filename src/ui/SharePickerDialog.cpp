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
    setStyleSheet(
        "SharePickerDialog { background: #1c1c1a; }"
        "QLabel { color: #e4e0da; }"
        "QTabWidget::pane { border: 1px solid #3a3a36; background: #1c1c1a; }"
        "QTabBar::tab { background: #2a2a26; color: #8a8680; padding: 8px 16px; border: none; }"
        "QTabBar::tab:selected { background: #3a3a36; color: #e4e0da; }"
        "QListWidget { background: #1c1c1a; color: #e4e0da; border: none; outline: none; }"
        "QListWidget::item { padding: 8px 12px; border-radius: 6px; }"
        "QListWidget::item:selected { background: #2a4a46; }"
        "QListWidget::item:hover { background: #2a2a26; }"
    );

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(16, 12, 16, 12);

    auto *title = new QLabel("Choose what to share", this);
    title->setStyleSheet("font-size: 16px; font-weight: bold; color: #e4e0da;");
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
    m_shareBtn->setStyleSheet(
        "QPushButton { background: #14b8a6; color: white; border: none; border-radius: 8px;"
        " font-size: 14px; font-weight: bold; padding: 10px 24px; }"
        "QPushButton:hover { background: #3ed4c6; }"
        "QPushButton:disabled { background: #3a3a36; color: #6a6660; }");
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

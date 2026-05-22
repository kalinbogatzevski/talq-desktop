#include "core/NotificationManager.h"
#include <QApplication>
#include <QWidget>
#include <QIcon>
#include <QFile>
#include <QActionGroup>
#include <QPainter>
#include <QPixmap>
#include <QFont>
#include <QFontMetrics>
#include <QSettings>
#include <QVector>
#include <QDebug>

#ifdef Q_OS_WIN
#include <windows.h>
#include <mmsystem.h>
#include <shobjidl.h>
#endif

NotificationManager::NotificationManager(QObject *parent)
    : QObject(parent)
{
    // Resolve the persisted sound id, with a one-shot migration from the
    // pre-0.33 key "soundMode" ("internal"→"chime"). After this block
    // m_soundId is one of none/system/<known tone> and Notifications/soundId
    // is written.
    QSettings s;
    s.beginGroup("Notifications");
    if (s.contains("soundId")) {
        m_soundId = s.value("soundId").toString();
    } else if (s.contains("soundMode")) {
        const QString old = s.value("soundMode").toString();
        if (old == "system")    m_soundId = "system";
        else if (old == "none") m_soundId = "none";
        else                    m_soundId = "chime";  // includes "internal"
        s.setValue("soundId", m_soundId);
    } else {
        m_soundId = "chime";
        s.setValue("soundId", m_soundId);
    }
    s.endGroup();

    loadSoundForId(m_soundId);

    m_baseIcon = QPixmap(":/logo.png").scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation);

    setupTrayIcon();
}

QVector<QPair<QString, QString>> NotificationManager::bundledTones()
{
    // Order = order shown in Settings combo + tray submenu. Default = first.
    return {
        { QStringLiteral("chime"),  QStringLiteral("Chime")  },
        { QStringLiteral("pop"),    QStringLiteral("Pop")    },
        { QStringLiteral("ding"),   QStringLiteral("Ding")   },
        { QStringLiteral("notify"), QStringLiteral("Notify") },
        { QStringLiteral("soft"),   QStringLiteral("Soft")   },
        { QStringLiteral("tone"),   QStringLiteral("Tone")   },
    };
}

void NotificationManager::loadSoundForId(const QString &id)
{
    if (id == QLatin1String("none") || id == QLatin1String("system")) {
        m_wavData.clear();
        return;
    }
    bool known = false;
    for (const auto &t : bundledTones())
        if (t.first == id) { known = true; break; }
    if (!known) {
        qWarning() << "NotificationManager: unknown soundId" << id
                   << "— falling back to chime";
        QSettings s;
        s.beginGroup("Notifications");
        s.setValue("soundId", "chime");
        s.endGroup();
        m_soundId = QStringLiteral("chime");
    }
    QFile f(QStringLiteral(":/sounds/%1.wav").arg(m_soundId == id ? id : m_soundId));
    if (f.open(QIODevice::ReadOnly)) {
        m_wavData = f.readAll();
    } else {
        qWarning() << "NotificationManager: cannot open" << f.fileName();
        m_wavData.clear();
    }
}

NotificationManager::~NotificationManager()
{
    delete m_trayMenu;
#ifdef Q_OS_WIN
    if (m_taskbar) {
        m_taskbar->Release();
        m_taskbar = nullptr;
    }
#endif
}

void NotificationManager::setWindow(QWidget *window)
{
    m_window = window;
}

void NotificationManager::setupTrayIcon()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable())
        return;

    m_trayIcon = new QSystemTrayIcon(this);
    m_trayIcon->setIcon(QIcon(":/logo.png"));
    m_trayIcon->setToolTip("TalQ");

    m_trayMenu = new QMenu();

    auto *showAction = m_trayMenu->addAction("Show TalQ");
    connect(showAction, &QAction::triggered, this, [this]() {
        emit showRequested();
    });

    m_trayMenu->addSeparator();

    // Sound submenu — fed from bundledTones() so the tray list and the
    // Settings combo never drift apart.
    auto *soundMenu = m_trayMenu->addMenu("Sound");
    auto *soundGroup = new QActionGroup(soundMenu);
    auto addSoundAction = [this, soundMenu, soundGroup]
                          (const QString &id, const QString &label) {
        auto *a = soundMenu->addAction(label);
        a->setCheckable(true);
        a->setChecked(m_soundId == id);
        a->setActionGroup(soundGroup);
        connect(a, &QAction::triggered, this, [this, id]() { setSoundId(id); });
    };
    addSoundAction("none",   "None");
    addSoundAction("system", "System default");
    soundMenu->addSeparator();
    for (const auto &t : bundledTones())
        addSoundAction(t.first, t.second);

    auto *notifAction = m_trayMenu->addAction("Desktop notifications");
    notifAction->setCheckable(true);
    notifAction->setChecked(m_notificationsEnabled);
    connect(notifAction, &QAction::toggled, this, &NotificationManager::setNotificationsEnabled);

    m_trayMenu->addSeparator();

    auto *quitAction = m_trayMenu->addAction("Quit");
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);

    m_trayIcon->setContextMenu(m_trayMenu);

    connect(m_trayIcon, &QSystemTrayIcon::activated,
            this, &NotificationManager::onTrayActivated);

    m_trayIcon->show();
}

void NotificationManager::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
        emit showRequested();
    }
}

void NotificationManager::playInternalSound()
{
#ifdef Q_OS_WIN
    if (!m_wavData.isEmpty()) {
        // Play from memory — bypasses Windows DND/Focus Assist
        PlaySoundA(m_wavData.constData(), NULL, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
#endif
}

void NotificationManager::playSystemSound()
{
#ifdef Q_OS_WIN
    // Uses Windows system notification sound — respects DND/Focus Assist
    PlaySoundW(L"SystemNotification", NULL, SND_ALIAS | SND_ASYNC | SND_NODEFAULT);
#endif
}

void NotificationManager::notify(const QString &title, const QString &message, bool alwaysSound, const QString &token)
{
    bool windowActive = m_window && m_window->isActiveWindow();

    // Play sound: always for cross-chat, only when unfocused for active chat
    if (alwaysSound || !windowActive) {
        if (m_soundId == "system") {
            playSystemSound();
        } else if (m_soundId != "none") {
            playInternalSound();  // m_wavData holds the selected tone bytes
        }
    }

    if (!m_notificationsEnabled) return;

    // Always show popup for cross-chat messages (alwaysSound = true means different conversation)
    // Only skip for messages in the active conversation when window is focused
    if (alwaysSound || !windowActive) {
        emit desktopPopupRequested(title, message, token);
    }
}

void NotificationManager::setNotifStyle(const QString &style)
{
    if (m_notifStyle != style) {
        m_notifStyle = style;
        emit notifStyleChanged();
    }
}

void NotificationManager::clearNotifications()
{
    m_unreadCount = 0;
    if (m_trayIcon) {
        m_trayIcon->setToolTip("TalQ");
    }
}

void NotificationManager::updateUnreadCount(int count)
{
    if (count == m_unreadCount) return;
    m_unreadCount = count;

#ifdef Q_OS_WIN
    updateTaskbarOverlay(count);
#endif

    if (!m_trayIcon) return;

    if (count > 0) {
        m_trayIcon->setToolTip(QString("TalQ — %1 unread").arg(count));

        // Paint badge on icon
        QPixmap icon = m_baseIcon;
        QPainter p(&icon);
        p.setRenderHint(QPainter::Antialiasing);

        // Red badge circle
        QString text = count > 99 ? "99+" : QString::number(count);
        QFont font;
        font.setPixelSize(count > 9 ? 11 : 13);
        font.setBold(true);
        QFontMetrics fm(font);
        int badgeW = qMax(16, fm.horizontalAdvance(text) + 6);
        int badgeH = 16;
        int bx = icon.width() - badgeW;
        int by = 0;

        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#e06060"));
        p.drawRoundedRect(bx, by, badgeW, badgeH, 8, 8);

        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(QRect(bx, by, badgeW, badgeH), Qt::AlignCenter, text);
        p.end();

        m_trayIcon->setIcon(QIcon(icon));
    } else {
        m_trayIcon->setToolTip("TalQ");
        m_trayIcon->setIcon(QIcon(":/logo.png"));
    }
}

#ifdef Q_OS_WIN
// QImage → HICON via CreateIconIndirect. Caller owns the returned HICON and
// must call DestroyIcon(). We feed CreateDIBSection with BI_BITFIELDS to get
// 32bpp ARGB with explicit channel masks; this is the path Windows actually
// alpha-blends correctly when used as a taskbar overlay icon.
static HICON QImageToHICON(const QImage &src)
{
    QImage img = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = img.width();
    bi.bV5Height = -img.height();   // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask   = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask  = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    HDC hdc = GetDC(nullptr);
    void *bits = nullptr;
    HBITMAP hbmColor = CreateDIBSection(hdc, reinterpret_cast<BITMAPINFO*>(&bi),
                                         DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (!hbmColor || !bits) {
        if (hbmColor) DeleteObject(hbmColor);
        return nullptr;
    }
    memcpy(bits, img.constBits(), img.sizeInBytes());

    // Empty 1bpp mask — Windows ignores it for 32bpp icons but ICONINFO requires it.
    HBITMAP hbmMask = CreateBitmap(img.width(), img.height(), 1, 1, nullptr);

    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.hbmColor = hbmColor;
    ii.hbmMask = hbmMask;
    HICON icon = CreateIconIndirect(&ii);

    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    return icon;
}

void NotificationManager::updateTaskbarOverlay(int count)
{
    if (!m_window) return;

    // Lazily create the ITaskbarList3 instance. Qt's windows platform plugin
    // initializes COM in apartment-threaded mode for us, so CoCreateInstance
    // just works here.
    if (!m_taskbar) {
        HRESULT hr = CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_ITaskbarList3, reinterpret_cast<void**>(&m_taskbar));
        if (FAILED(hr) || !m_taskbar) {
            qWarning() << "NotificationManager: failed to create ITaskbarList3, hr=" << hr;
            return;
        }
        if (FAILED(m_taskbar->HrInit())) {
            m_taskbar->Release();
            m_taskbar = nullptr;
            return;
        }
    }

    HWND hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (!hwnd) return;

    if (count <= 0) {
        m_taskbar->SetOverlayIcon(hwnd, nullptr, L"");
        return;
    }

    // Render a 16×16 red badge with the count. Windows scales the overlay
    // to fit the taskbar icon, so 16×16 is plenty for HiDPI displays too.
    QImage img(16, 16, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor("#e06060"));
        p.drawEllipse(QRect(0, 0, 16, 16));

        const QString text = count > 99 ? QStringLiteral("99+") : QString::number(count);
        QFont font;
        font.setPixelSize(count > 99 ? 8 : (count > 9 ? 10 : 11));
        font.setBold(true);
        p.setFont(font);
        p.setPen(Qt::white);
        p.drawText(img.rect(), Qt::AlignCenter, text);
    }

    HICON icon = QImageToHICON(img);
    const std::wstring desc = QString("%1 unread").arg(count).toStdWString();
    m_taskbar->SetOverlayIcon(hwnd, icon, desc.c_str());
    if (icon) DestroyIcon(icon);
}
#endif

void NotificationManager::setSoundId(const QString &id)
{
    if (m_soundId == id) return;
    m_soundId = id;
    loadSoundForId(m_soundId);
    QSettings s;
    s.beginGroup("Notifications");
    s.setValue("soundId", m_soundId);
    s.endGroup();
    emit soundIdChanged();
}

void NotificationManager::playCurrentSound()
{
    // Audition helper for the Settings dropdown — silent for none/system
    // pick (system would just fire the OS sound; we keep audition to the
    // bundled tones so it's a clear "this is the tone" preview).
    if (m_soundId == "none" || m_soundId == "system") return;
    playInternalSound();
}

void NotificationManager::setNotificationsEnabled(bool v)
{
    if (m_notificationsEnabled != v) {
        m_notificationsEnabled = v;
        emit notificationsEnabledChanged();
    }
}

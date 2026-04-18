#include "HeaderPainter.h"
#include "core/ApiClient.h"
#include <QPainter>
#include <QPainterPath>
#include <QPaintEvent>
#include <QMouseEvent>
#include <QHoverEvent>
#include <QNetworkReply>
#include <QFontMetrics>
#include <QCursor>
#include <QToolTip>
#include <QtMath>

// ═══════════════════════════════════════════════════════
// Constructor
// ═══════════════════════════════════════════════════════

HeaderPainter::HeaderPainter(QWidget *parent)
    : QWidget(parent)
    , m_theme(m_darkMode, 1.0)
{
    setAttribute(Qt::WA_Hover);
    setMouseTracking(true);
    setFixedHeight(HeaderHeight);
}

// ═══════════════════════════════════════════════════════
// Property setters (all follow same pattern: guard, set, update, emit)
// ═══════════════════════════════════════════════════════

void HeaderPainter::setConversationName(const QString &v) {
    if (m_conversationName == v) return;
    m_conversationName = v;
    update();
}

void HeaderPainter::setConversationUserId(const QString &v) {
    if (m_conversationUserId == v) return;
    m_conversationUserId = v;
    update();
}

void HeaderPainter::setAvatarImage(const QImage &img) {
    if (!img.isNull() && !m_conversationUserId.isEmpty()) {
        m_avatarCache[m_conversationUserId] = img;
        update();
    }
}

void HeaderPainter::setConversationType(int v) {
    if (m_conversationType == v) return;
    m_conversationType = v;
    update();
}

void HeaderPainter::setPeerStatus(const QString &state, const QString &message, const QString &icon)
{
    if (m_peerStatus == state && m_peerStatusMessage == message && m_peerStatusIcon == icon)
        return;
    m_peerStatus = state;
    m_peerStatusMessage = message;
    m_peerStatusIcon = icon;
    update();
}

void HeaderPainter::setActiveThreadId(int v) {
    if (m_activeThreadId == v) return;
    m_activeThreadId = v;
    update();
}

void HeaderPainter::setActiveThreadTitle(const QString &v) {
    if (m_activeThreadTitle == v) return;
    m_activeThreadTitle = v;
    update();
}

void HeaderPainter::setActiveThreadColor(int v) {
    if (m_activeThreadColor == v) return;
    m_activeThreadColor = v;
    update();
}

void HeaderPainter::setIsInTopicMode(bool v) {
    if (m_isInTopicMode == v) return;
    m_isInTopicMode = v;
    update();
}

void HeaderPainter::setSidebarSqueezed(bool v) {
    if (m_sidebarSqueezed == v) return;
    m_sidebarSqueezed = v;
    update();
}

void HeaderPainter::setConversationToken(const QString &v) {
    if (m_conversationToken == v) return;
    m_conversationToken = v;
    update();
}

void HeaderPainter::setMessageCount(int v) {
    if (m_messageCount == v) return;
    m_messageCount = v;
    update();
}

void HeaderPainter::setLoading(bool v) {
    if (m_loading == v) return;
    m_loading = v;
    update();
}

void HeaderPainter::setTypingUser(const QString &v) {
    if (m_typingUser == v) return;
    m_typingUser = v;
    update();
}

void HeaderPainter::setIsTyping(bool v) {
    if (m_isTyping == v) return;
    m_isTyping = v;
    update();
}

void HeaderPainter::setCallState(int v) {
    if (m_callState == v) return;
    m_callState = v;
    update();
}

void HeaderPainter::setCallDuration(int v) {
    if (m_callDuration == v) return;
    m_callDuration = v;
    update();
}

void HeaderPainter::setCallsAvailable(bool v) {
    if (m_callsAvailable == v) return;
    m_callsAvailable = v;
    update();
}

void HeaderPainter::setCallsUnavailableReason(const QString &v) {
    if (m_callsUnavailableReason == v) return;
    m_callsUnavailableReason = v;
    update();
}

void HeaderPainter::setDarkMode(bool v) {
    if (m_darkMode == v) return;
    m_darkMode = v;
    m_theme = PainterTheme(m_darkMode, 1.0);
    update();
}

void HeaderPainter::setApi(ApiClient *api) {
    if (api == m_api) return;
    m_api = api;
}

// ═══════════════════════════════════════════════════════
// PAINT
// ═══════════════════════════════════════════════════════

void HeaderPainter::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    QPainter *painter = &p;

    const qreal w = width();
    const qreal h = height();
    const qreal padLeft = PainterTheme::spacingLarge;
    const qreal padRight = PainterTheme::spacingXLarge;
    const qreal spacing = PainterTheme::spacingSmall;

    // Background
    painter->fillRect(QRectF(0, 0, w, h), m_theme.bgPrimary);

    // Bottom border
    painter->setPen(Qt::NoPen);
    painter->setBrush(m_theme.divider);
    painter->drawRect(QRectF(0, h - 1, w, 1));

    // Clear hit-test rects
    m_expandBtnRect = QRectF();
    m_backBtnRect = QRectF();
    m_audioCallRect = QRectF();
    m_videoCallRect = QRectF();
    m_searchBtnRect = QRectF();

    qreal x = padLeft;

    // ── Expand sidebar button (visible when sidebar squeezed and conversation active) ──
    bool showExpandBtn = m_sidebarSqueezed && !m_conversationName.isEmpty();
    if (showExpandBtn) {
        // Draw a smaller 30px button for the arrow
        qreal smallBtn = 30;
        qreal btnYSmall = (h - smallBtn) / 2.0;
        m_expandBtnRect = QRectF(x, btnYSmall, smallBtn, smallBtn);
        paintBackButton(painter, m_expandBtnRect, m_hoveredButton == 0);
        x += smallBtn + spacing;
    }

    // ── Back button (visible when viewing a thread) ──
    bool showBackBtn = m_activeThreadId > 0;
    if (showBackBtn) {
        qreal smallBtn = 30;
        qreal btnYSmall = (h - smallBtn) / 2.0;
        m_backBtnRect = QRectF(x, btnYSmall, smallBtn, smallBtn);
        paintBackButton(painter, m_backBtnRect, m_hoveredButton == 1);
        x += smallBtn + spacing;
    }

    // ── Topic color dot ──
    bool isViewingTopic = m_isInTopicMode && m_activeThreadId > 0;
    if (isViewingTopic) {
        qreal dotY = (h - TopicDotSize) / 2.0;
        painter->setPen(Qt::NoPen);
        painter->setBrush(PainterTheme::topicColor(m_activeThreadColor));
        painter->drawEllipse(QRectF(x, dotY, TopicDotSize, TopicDotSize));
        x += TopicDotSize + spacing;
    }

    // ── Avatar ──
    if (!m_conversationName.isEmpty()) {
        qreal avatarY = (h - AvatarSize) / 2.0;
        QRectF avatarRect(x, avatarY, AvatarSize, AvatarSize);
        paintAvatar(painter, avatarRect);
        x += AvatarSize + spacing;
    }

    // ── Right side: call buttons, loading indicators ──
    // Compute right-side content first so we know where text area ends
    qreal rightX = w - padRight;

    // Loading spinner placeholder (simple text)
    if (m_loading && m_messageCount == 0) {
        // BusyIndicator equivalent: just show "..." for now
        QFont smallFont;
        smallFont.setPixelSize(m_theme.fontSizeTiny);
        painter->setFont(smallFont);
        painter->setPen(m_theme.accent);
        QString loadText = QStringLiteral("\u2022\u2022\u2022");
        QFontMetrics fm(smallFont);
        qreal textW = fm.horizontalAdvance(loadText);
        rightX -= textW + spacing;
        painter->drawText(QRectF(rightX, 0, textW, h), Qt::AlignCenter, loadText);
    } else if (m_loading && m_messageCount > 0) {
        // Sync indicator
        QFont smallFont;
        smallFont.setPixelSize(14);
        painter->setFont(smallFont);
        painter->setPen(m_theme.textMuted);
        QString syncText = QStringLiteral("\u21BB");  // ↻
        QFontMetrics fm(smallFont);
        qreal textW = fm.horizontalAdvance(syncText);
        rightX -= textW + spacing;
        painter->drawText(QRectF(rightX, 0, textW, h), Qt::AlignCenter, syncText);
    }

    // Active call indicator
    if (m_callState > 0) {
        int m = m_callDuration / 60;
        int s = m_callDuration % 60;
        QString durText = QStringLiteral("\U0001F4DE ") +
            (m < 10 ? QStringLiteral("0") : QString()) + QString::number(m) +
            QStringLiteral(":") +
            (s < 10 ? QStringLiteral("0") : QString()) + QString::number(s);
        QFont durFont;
        durFont.setPixelSize(m_theme.fontSizeTiny);
        QFontMetrics fm(durFont);
        qreal textW = fm.horizontalAdvance(durText);
        rightX -= textW + spacing;
        painter->setFont(durFont);
        painter->setPen(m_theme.online);
        painter->drawText(QRectF(rightX, 0, textW, h), Qt::AlignVCenter, durText);
    }

    // Calls unavailable label (1:1, not in call, calls not available)
    if (m_conversationType == 1 && !m_callsAvailable && m_callState == 0) {
        QString unavailText = QStringLiteral("Calls unavailable");
        QFont unavailFont;
        unavailFont.setPixelSize(m_theme.fontSizeTiny);
        QFontMetrics fm(unavailFont);
        qreal textW = fm.horizontalAdvance(unavailText);
        rightX -= textW + spacing;
        painter->setFont(unavailFont);
        painter->setPen(m_theme.textMuted);
        painter->drawText(QRectF(rightX, 0, textW, h), Qt::AlignVCenter, unavailText);
    }

    // Call buttons (1:1, not in call, calls available)
    if (m_conversationType == 1 && m_callState == 0 && m_callsAvailable) {
        // Video call button
        qreal btnY = (h - ButtonSize) / 2.0;
        rightX -= ButtonSize;
        m_videoCallRect = QRectF(rightX, btnY, ButtonSize, ButtonSize);
        paintCallButton(painter, m_videoCallRect, m_theme.accent,
                        QStringLiteral("\U0001F4F9"), m_hoveredButton == 3);  // 📹 camera
        rightX -= spacing;

        // Audio call button
        rightX -= ButtonSize;
        m_audioCallRect = QRectF(rightX, btnY, ButtonSize, ButtonSize);
        paintCallButton(painter, m_audioCallRect, m_theme.success,
                        QStringLiteral("\u260E"), m_hoveredButton == 2);  // ☎ as phone icon
        rightX -= spacing;
    }

    // Search button (shown when a conversation is active)
    if (!m_conversationToken.isEmpty()) {
        qreal btnY = (h - ButtonSize) / 2.0;
        rightX -= ButtonSize;
        m_searchBtnRect = QRectF(rightX, btnY, ButtonSize, ButtonSize);
        paintCallButton(painter, m_searchBtnRect, m_theme.textSecondary,
                        QStringLiteral("\U0001F50D"), m_hoveredButton == 4);  // 🔍
        rightX -= spacing;
    }

    // ── Text area: name + status ──
    qreal textLeft = x;
    qreal textRight = rightX - spacing;
    qreal textW = textRight - textLeft;
    if (textW < 20) return;  // too narrow to draw text

    // Title text
    QString titleText;
    if (m_activeThreadId > 0) {
        titleText = m_activeThreadTitle;
    } else if (!m_conversationName.isEmpty()) {
        titleText = m_conversationName;
    } else {
        titleText = QStringLiteral("Select a conversation");
    }

    QFont titleFont;
    titleFont.setPixelSize(m_theme.fontSizeLarge);
    if (!m_conversationName.isEmpty()) {
        titleFont.setWeight(QFont::DemiBold);
    } else if (m_activeThreadId <= 0) {
        titleFont.setItalic(true);  // placeholder "Select a conversation"
    }
    QFontMetrics titleFM(titleFont);

    // Decide if we have a subtitle
    bool hasSubtitle = false;
    QString subtitleText;
    QColor subtitleColor = m_theme.textMuted;
    bool subtitleItalic = false;

    if (m_isTyping) {
        hasSubtitle = true;
        subtitleText = m_typingUser + QStringLiteral(" is typing...");
        subtitleColor = m_theme.accent;
        subtitleItalic = true;
    } else if (m_conversationType == 1 && !m_peerStatus.isEmpty()) {
        hasSubtitle = true;
        // Prefer rich status message/icon over plain state label
        if (!m_peerStatusMessage.isEmpty()) {
            subtitleText = m_peerStatusIcon.isEmpty()
                ? m_peerStatusMessage
                : m_peerStatusIcon + QLatin1Char(' ') + m_peerStatusMessage;
            subtitleColor = m_theme.textMuted;
        } else if (m_peerStatus == QStringLiteral("online")) {
            subtitleText = tr("Online");
            subtitleColor = m_theme.online;
        } else if (m_peerStatus == QStringLiteral("away")) {
            subtitleText = tr("Away");
            subtitleColor = QColor("#f0a050");  // warning color
        } else if (m_peerStatus == QStringLiteral("dnd")) {
            subtitleText = tr("Do not disturb");
            subtitleColor = m_theme.danger;
        } else {
            subtitleText = tr("Offline");
            subtitleColor = m_theme.textMuted;
        }
    } else if (isViewingTopic && !m_isTyping) {
        hasSubtitle = true;
        subtitleText = m_conversationName + QStringLiteral(" \u00B7 ") +
                       QString::number(m_messageCount) + QStringLiteral(" messages");
        subtitleColor = m_theme.textSecondary;
    }

    if (hasSubtitle) {
        // Two-line layout: title at ~1/3 height, subtitle below
        qreal titleH = titleFM.height();
        qreal subtitleFontSize = m_theme.fontSizeTiny;
        QFont subFont;
        subFont.setPixelSize(subtitleFontSize);
        subFont.setItalic(subtitleItalic);
        QFontMetrics subFM(subFont);
        qreal subH = subFM.height();
        qreal totalH = titleH + subH + 1;
        qreal topY = (h - totalH) / 2.0;

        // Title
        QColor titleColor = m_conversationName.isEmpty() ? m_theme.textMuted : m_theme.textPrimary;
        QString elidedTitle = titleFM.elidedText(titleText, Qt::ElideRight, static_cast<int>(textW));
        painter->setFont(titleFont);
        painter->setPen(titleColor);
        painter->drawText(QRectF(textLeft, topY, textW, titleH),
                          Qt::AlignLeft | Qt::AlignVCenter, elidedTitle);

        // Subtitle
        QString elidedSub = subFM.elidedText(subtitleText, Qt::ElideRight, static_cast<int>(textW));
        painter->setFont(subFont);
        painter->setPen(subtitleColor);
        painter->drawText(QRectF(textLeft, topY + titleH + 1, textW, subH),
                          Qt::AlignLeft | Qt::AlignVCenter, elidedSub);
    } else {
        // Single line: centered vertically
        QColor titleColor = m_conversationName.isEmpty() ? m_theme.textMuted : m_theme.textPrimary;
        QString elidedTitle = titleFM.elidedText(titleText, Qt::ElideRight, static_cast<int>(textW));
        painter->setFont(titleFont);
        painter->setPen(titleColor);
        painter->drawText(QRectF(textLeft, 0, textW, h),
                          Qt::AlignLeft | Qt::AlignVCenter, elidedTitle);
    }
}

// ═══════════════════════════════════════════════════════
// Painting helpers
// ═══════════════════════════════════════════════════════

void HeaderPainter::paintBackButton(QPainter *p, const QRectF &rect, bool hovered)
{
    // Rounded rect background on hover
    if (hovered) {
        p->setPen(Qt::NoPen);
        p->setBrush(m_theme.bgHover);
        p->drawRoundedRect(rect, rect.width() / 2, rect.height() / 2);
    }

    // Draw arrow-left: a simple < chevron
    p->setPen(QPen(m_theme.accent, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p->setBrush(Qt::NoBrush);
    qreal cx = rect.center().x();
    qreal cy = rect.center().y();
    qreal arrowW = 7;
    qreal arrowH = 10;
    // Left-pointing chevron
    QPointF pts[3] = {
        QPointF(cx - arrowW / 2, cy),
        QPointF(cx + arrowW / 2, cy - arrowH / 2),
        QPointF(cx + arrowW / 2, cy + arrowH / 2)
    };
    p->drawLine(pts[1], pts[0]);
    p->drawLine(pts[0], pts[2]);
}

void HeaderPainter::paintCallButton(QPainter *p, const QRectF &rect, const QColor &iconColor,
                                     const QString &icon, bool hovered)
{
    // Subtle transparent button — icon only, hover highlight
    if (hovered) {
        p->setPen(Qt::NoPen);
        p->setBrush(QColor(255, 255, 255, 20));
        p->drawRoundedRect(rect, rect.width() / 2, rect.height() / 2);
    }

    // Icon in muted color, brighter on hover
    QFont iconFont;
    iconFont.setPixelSize(16);
    p->setFont(iconFont);
    p->setPen(hovered ? iconColor : m_theme.textSecondary);
    p->drawText(rect, Qt::AlignCenter, icon);
}

void HeaderPainter::paintAvatar(QPainter *p, const QRectF &rect)
{
    int size = static_cast<int>(rect.width());
    QImage img = fetchAvatar(m_conversationUserId);

    if (!img.isNull()) {
        if (img.width() != size)
            img = img.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        p->drawImage(rect, img);
    } else {
        // Fallback: colored circle with initial
        QColor bgColor = PainterTheme::authorColor(
            m_conversationUserId.isEmpty() ? m_conversationName : m_conversationUserId);
        p->setPen(Qt::NoPen);
        p->setBrush(bgColor);
        p->drawEllipse(rect);

        QChar initial = m_conversationName.isEmpty() ? QChar('?') : m_conversationName.at(0).toUpper();
        QFont initFont;
        initFont.setPixelSize(size / 2);
        initFont.setWeight(QFont::DemiBold);
        p->setPen(Qt::white);
        p->setFont(initFont);
        p->drawText(rect, Qt::AlignCenter, QString(initial));
    }
}

// ═══════════════════════════════════════════════════════
// Avatar loading
// ═══════════════════════════════════════════════════════

QImage HeaderPainter::fetchAvatar(const QString &userId)
{
    if (userId.isEmpty())
        return QImage();

    auto it = m_avatarCache.find(userId);
    if (it != m_avatarCache.end())
        return it.value();

    requestAvatar(userId);
    return QImage();
}

void HeaderPainter::requestAvatar(const QString &userId)
{
    if (m_avatarPending.contains(userId))
        return;
    if (!m_api)
        return;

    m_avatarPending.insert(userId);

    // For 1:1, use user avatar. For groups, use room avatar.
    QString url;
    if (m_conversationType == 1 && !userId.isEmpty()) {
        url = QStringLiteral("/index.php/avatar/") + userId + QStringLiteral("/64");
    } else {
        url = QStringLiteral("/ocs/v2.php/apps/spreed/api/v1/room/") +
              m_conversationToken + QStringLiteral("/avatar?v=1");
    }

    QNetworkReply *reply = m_api->getAbsoluteUrl(url);

    connect(reply, &QNetworkReply::finished, this, [this, reply, userId]() {
        reply->deleteLater();
        m_avatarPending.remove(userId);

        if (reply->error() != QNetworkReply::NoError) {
            m_avatarCache[userId] = QImage();
            return;
        }

        QByteArray data = reply->readAll();
        QImage img;
        if (!img.loadFromData(data)) {
            m_avatarCache[userId] = QImage();
            return;
        }

        m_avatarCache[userId] = PainterTheme::cropToCircle(img, AvatarSize);
        update();
    });
}

// ═══════════════════════════════════════════════════════
// Input handling
// ═══════════════════════════════════════════════════════

int HeaderPainter::buttonAtPos(const QPointF &pos) const
{
    if (m_expandBtnRect.isValid() && m_expandBtnRect.contains(pos)) return 0;
    if (m_backBtnRect.isValid() && m_backBtnRect.contains(pos)) return 1;
    if (m_audioCallRect.isValid() && m_audioCallRect.contains(pos)) return 2;
    if (m_videoCallRect.isValid() && m_videoCallRect.contains(pos)) return 3;
    if (m_searchBtnRect.isValid() && m_searchBtnRect.contains(pos)) return 4;
    return -1;
}

void HeaderPainter::mousePressEvent(QMouseEvent *event)
{
    event->accept();
}

void HeaderPainter::mouseReleaseEvent(QMouseEvent *event)
{
    int btn = buttonAtPos(event->position());
    switch (btn) {
    case 0: emit expandSidebarClicked(); break;
    case 1: emit backClicked(); break;
    case 2: emit audioCallClicked(); break;
    case 3: emit videoCallClicked(); break;
    case 4: emit searchRequested(); break;
    default: break;
    }
    event->accept();
}

bool HeaderPainter::event(QEvent *e)
{
    if (e->type() == QEvent::HoverMove) {
        auto *he = static_cast<QHoverEvent *>(e);
        int btn = buttonAtPos(he->position());
        if (btn != m_hoveredButton) {
            m_hoveredButton = btn;
            if (btn >= 0)
                setCursor(QCursor(Qt::PointingHandCursor));
            else
                setCursor(QCursor(Qt::ArrowCursor));
            update();

            // Tooltips
            QString tip;
            switch (btn) {
            case 0: tip = "Expand sidebar"; break;
            case 1: tip = "Back"; break;
            case 2: tip = "Audio call"; break;
            case 3: tip = "Video call"; break;
            case 4: tip = "Search in conversation"; break;
            }
            if (!tip.isEmpty())
                QToolTip::showText(mapToGlobal(he->position().toPoint()), tip, this);
            else
                QToolTip::hideText();
        }
        return true;
    }
    if (e->type() == QEvent::HoverLeave) {
        if (m_hoveredButton != -1) {
            m_hoveredButton = -1;
            setCursor(QCursor(Qt::ArrowCursor));
            update();
        }
        return true;
    }
    return QWidget::event(e);
}

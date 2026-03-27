#include "LayoutEngine.h"
#include "models/MessageListModel.h"
#include <QFontMetrics>
#include <QTextDocument>
#include <QtMath>

std::shared_ptr<QTextDocument> LayoutEngine::createBodyDoc(
    const QString &html, qreal maxWidth, const PainterTheme &theme)
{
    auto doc = std::make_shared<QTextDocument>();
    doc->setDefaultFont(theme.bodyFont());
    doc->setTextWidth(maxWidth);
    doc->setDocumentMargin(0);

    // Convert newlines to <br> like QML does, then set as HTML
    QString processed = html;
    processed.replace(QLatin1String("\n"), QLatin1String("<br>"));
    doc->setHtml(processed);

    return doc;
}

MessageLayout LayoutEngine::computeLayout(
    QAbstractListModel *model,
    int modelRow,
    qreal width,
    const PainterTheme &theme,
    qreal startY,
    const QString &myUserId,
    const QString &prevActorId,
    qint64 prevTimestamp,
    bool prevIsSystem)
{
    MessageLayout ml;
    ml.modelRow = modelRow;
    ml.totalY = startY;

    // Fetch data from model
    auto idx = model->index(modelRow);
    ml.messageId    = model->data(idx, MessageListModel::IdRole).toInt();
    ml.actorName    = model->data(idx, MessageListModel::ActorNameRole).toString();
    ml.actorId      = model->data(idx, MessageListModel::ActorIdRole).toString();
    ml.bodyHtml     = model->data(idx, MessageListModel::MessageTextRole).toString();
    ml.timeString   = model->data(idx, MessageListModel::TimeStringRole).toString();
    ml.isSystem     = model->data(idx, MessageListModel::IsSystemRole).toBool();
    ml.showDateSep  = model->data(idx, MessageListModel::ShowDateSeparatorRole).toBool();
    ml.dateString   = model->data(idx, MessageListModel::DateStringRole).toString();
    ml.isRead       = model->data(idx, MessageListModel::IsReadRole).toBool();
    ml.sendStatus   = model->data(idx, MessageListModel::SendStatusRole).toString();
    ml.replyToAuthor = model->data(idx, MessageListModel::ReplyToAuthorRole).toString();
    ml.replyToText  = model->data(idx, MessageListModel::ReplyToTextRole).toString();
    ml.reactions    = model->data(idx, MessageListModel::ReactionsRole).toString();

    // File attachment
    ml.hasFile  = model->data(idx, MessageListModel::HasFileRole).toBool();
    ml.fileId   = model->data(idx, MessageListModel::FileIdRole).toInt();
    ml.fileName = model->data(idx, MessageListModel::FileNameRole).toString();
    ml.fileMime = model->data(idx, MessageListModel::FileMimeRole).toString();

    ml.isOwn = (ml.actorId == myUserId);

    // Grouping: same author, within 300s, neither is system
    ml.isGrouped = !ml.isSystem && !prevIsSystem
        && !ml.actorId.isEmpty() && ml.actorId == prevActorId
        && prevTimestamp > 0 && qAbs(model->data(idx, MessageListModel::TimestampRole).toLongLong() - prevTimestamp) < 300;

    // If showing date separator, never group
    if (ml.showDateSep)
        ml.isGrouped = false;

    const qreal margin = PainterTheme::spacingNormal;
    const qreal bubbleMaxWidth = width * 0.75;
    qreal y = startY;

    // ── Date separator ──
    if (ml.showDateSep) {
        QFontMetrics fm(theme.dateSepFont());
        int pillTextW = fm.horizontalAdvance(ml.dateString);
        int pillW = pillTextW + PainterTheme::spacingXLarge;

        ml.dateSepRect = QRectF(
            (width - pillW) / 2.0,
            y + (PainterTheme::dateSepHeight - PainterTheme::datePillHeight) / 2.0,
            pillW,
            PainterTheme::datePillHeight
        );
        ml.dateSepTextRect = ml.dateSepRect; // text centered inside pill

        y += PainterTheme::dateSepHeight;
    }

    // ── System message ──
    if (ml.isSystem) {
        QFontMetrics fm(theme.systemFont());
        int textH = fm.height();
        ml.bodyRect = QRectF(0, y + 8, width, textH);
        y += textH + 16;
        ml.totalHeight = y - startY;
        return ml;
    }

    // ── Spacing between messages ──
    y += ml.isGrouped ? PainterTheme::messageSpacingGrouped
                      : PainterTheme::messageSpacingNormal;

    // ── Own message ──
    if (ml.isOwn) {
        qreal bubbleX = width - bubbleMaxWidth - margin;
        qreal bubbleInnerW = bubbleMaxWidth - 2 * PainterTheme::spacingNormal;
        qreal innerY = y + 7; // top padding inside bubble

        // Reply quote
        if (!ml.replyToText.isEmpty()) {
            QFontMetrics fmTiny(theme.timeFont());
            int quoteH = fmTiny.height() * 2 + 12; // author + text + padding
            ml.quoteRect = QRectF(
                bubbleX + PainterTheme::spacingNormal,
                innerY,
                bubbleInnerW,
                quoteH
            );
            innerY += quoteH + 4;
        }

        // File attachment area
        if (ml.hasFile) {
            bool isImage = ml.fileMime.startsWith(QLatin1String("image/"));
            qreal fileH = isImage ? qMin(bubbleInnerW * 0.75, 300.0) : 44.0;
            ml.fileRect = QRectF(
                bubbleX + PainterTheme::spacingNormal,
                innerY,
                bubbleInnerW,
                fileH
            );
            innerY += fileH + 4;
        }

        // Body text
        ml.bodyDoc = createBodyDoc(ml.bodyHtml, bubbleInnerW, theme);
        qreal bodyH = ml.bodyDoc->size().height();
        ml.bodyRect = QRectF(
            bubbleX + PainterTheme::spacingNormal,
            innerY,
            bubbleInnerW,
            bodyH
        );
        innerY += bodyH + 2;

        // Reactions
        if (!ml.reactions.isEmpty()) {
            ml.reactBarRect = QRectF(
                bubbleX + PainterTheme::spacingNormal,
                innerY,
                bubbleInnerW,
                24
            );
            innerY += 28;
        }

        // Timestamp row
        QFontMetrics fmTime(theme.timeFont());
        int timeW = fmTime.horizontalAdvance(ml.timeString) + 20; // room for status icon
        int timeH = fmTime.height();
        ml.timeRect = QRectF(
            bubbleX + bubbleMaxWidth - PainterTheme::spacingNormal - timeW,
            innerY,
            timeW,
            timeH
        );
        innerY += timeH + 7; // bottom padding

        // Bubble rect
        ml.bubbleRect = QRectF(bubbleX, y, bubbleMaxWidth, innerY - y);

        y = innerY;
    }
    // ── Other person's message ──
    else {
        qreal contentX = margin;
        qreal avatarW = 0;

        if (!ml.isGrouped) {
            // Avatar
            ml.avatarRect = QRectF(
                margin,
                y,
                PainterTheme::avatarSize,
                PainterTheme::avatarSize
            );
            avatarW = PainterTheme::avatarSize + PainterTheme::avatarGap;
        } else {
            avatarW = PainterTheme::avatarSize + PainterTheme::avatarGap;
        }

        contentX = margin + avatarW;
        qreal contentW = bubbleMaxWidth - avatarW - margin;

        // Name + time row (non-grouped)
        if (!ml.isGrouped) {
            QFontMetrics fmName(theme.nameFont());
            int nameH = fmName.height();
            ml.nameRect = QRectF(contentX, y, contentW, nameH);
            // Time is placed after name text
            QFontMetrics fmTime(theme.timeFont());
            int nameTextW = fmName.horizontalAdvance(ml.actorName);
            ml.timeRect = QRectF(
                contentX + nameTextW + PainterTheme::spacingSmall,
                y + (nameH - fmTime.height()) / 2.0, // vertically center with name
                fmTime.horizontalAdvance(ml.timeString),
                fmTime.height()
            );
            y += nameH + 2;
        }

        // Reply quote
        if (!ml.replyToText.isEmpty()) {
            QFontMetrics fmTiny(theme.timeFont());
            int quoteH = fmTiny.height() * 2 + 12;
            ml.quoteRect = QRectF(contentX, y, contentW, quoteH);
            y += quoteH + 4;
        }

        // File attachment area
        if (ml.hasFile) {
            bool isImage = ml.fileMime.startsWith(QLatin1String("image/"));
            qreal fileH = isImage ? qMin(contentW * 0.75, 300.0) : 44.0;
            ml.fileRect = QRectF(contentX, y, contentW, fileH);
            y += fileH + 4;
        }

        // Body text
        ml.bodyDoc = createBodyDoc(ml.bodyHtml, contentW, theme);
        qreal bodyH = ml.bodyDoc->size().height();
        ml.bodyRect = QRectF(contentX, y, contentW, bodyH);
        y += bodyH;

        // Reactions
        if (!ml.reactions.isEmpty()) {
            ml.reactBarRect = QRectF(contentX, y + 2, contentW, 24);
            y += 28;
        }

        // Grouped messages: time below body
        if (ml.isGrouped) {
            QFontMetrics fmTime(theme.timeFont());
            ml.timeRect = QRectF(
                contentX,
                y + 2,
                fmTime.horizontalAdvance(ml.timeString),
                fmTime.height()
            );
            y += fmTime.height() + 4;
        }

        y += 2; // bottom padding
    }

    ml.totalHeight = y - startY;
    return ml;
}

#include "LayoutEngine.h"
#include "models/MessageListModel.h"
#include <QFontMetrics>
#include <QTextDocument>
#include <QRegularExpression>
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
    bool prevIsSystem,
    qreal imageAspectRatio)
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
        // Skip empty own messages (no text, no file, no reply) — prevents tiny green bar
        QString strippedBody = ml.bodyHtml;
        strippedBody.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
        strippedBody = strippedBody.trimmed();
        if (strippedBody.isEmpty() && !ml.hasFile && ml.replyToText.isEmpty()) {
            ml.totalHeight = ml.showDateSep ? (y - startY) : 0;
            return ml;
        }

        qreal maxInnerW = bubbleMaxWidth - 2 * PainterTheme::spacingNormal;

        // First pass: measure content to find actual needed width
        // Body text — layout with max width for wrapping, then get ideal width
        ml.bodyDoc = createBodyDoc(ml.bodyHtml, maxInnerW, theme);
        qreal bodyIdealW = ml.bodyDoc->idealWidth();
        qreal bodyH = ml.bodyDoc->size().height();

        // Timestamp
        QFontMetrics fmTime(theme.timeFont());
        int timeW = fmTime.horizontalAdvance(ml.timeString) + 20;
        int timeH = fmTime.height();

        // Determine actual bubble inner width: widest of body, time, quote, file
        qreal neededW = qMax(bodyIdealW, (qreal)timeW);
        if (!ml.replyToText.isEmpty()) {
            QFontMetrics fmTiny(theme.timeFont());
            qreal quoteTextW = fmTiny.horizontalAdvance(ml.replyToAuthor + ": " + ml.replyToText) + 20;
            neededW = qMax(neededW, qMin(quoteTextW, maxInnerW));
        }
        if (ml.hasFile) neededW = maxInnerW; // files use full width
        if (!ml.reactions.isEmpty()) neededW = qMax(neededW, 100.0);

        // Clamp: min 80, max bubbleMaxWidth inner
        qreal bubbleInnerW = qBound(80.0, neededW + 2, maxInnerW);
        qreal actualBubbleW = bubbleInnerW + 2 * PainterTheme::spacingNormal;
        qreal bubbleX = width - actualBubbleW - margin;

        // Re-layout body with actual width (may differ from max)
        if (qAbs(bubbleInnerW - maxInnerW) > 1) {
            ml.bodyDoc = createBodyDoc(ml.bodyHtml, bubbleInnerW, theme);
            bodyH = ml.bodyDoc->size().height();
        }

        qreal innerY = y + 7; // top padding inside bubble

        // Reply quote
        if (!ml.replyToText.isEmpty()) {
            QFontMetrics fmTiny(theme.timeFont());
            int quoteH = fmTiny.height() * 2 + 12;
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
            qreal fileH = isImage ? qMin(bubbleInnerW * imageAspectRatio, 300.0) : 44.0;
            ml.fileRect = QRectF(
                bubbleX + PainterTheme::spacingNormal,
                innerY,
                bubbleInnerW,
                fileH
            );
            innerY += fileH + 4;
        }

        // Body text
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
        ml.timeRect = QRectF(
            bubbleX + actualBubbleW - PainterTheme::spacingNormal - timeW,
            innerY,
            timeW,
            timeH
        );
        innerY += timeH + 7; // bottom padding

        // Bubble rect
        ml.bubbleRect = QRectF(bubbleX, y, actualBubbleW, innerY - y);
        ml.contentRight = bubbleX; // own messages: hover bar goes LEFT of bubble

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
        qreal maxContentW = bubbleMaxWidth - avatarW - margin;

        // First pass: measure body to get ideal width
        ml.bodyDoc = createBodyDoc(ml.bodyHtml, maxContentW, theme);
        qreal bodyIdealW = ml.bodyDoc->idealWidth();
        qreal bodyH = ml.bodyDoc->size().height();

        // Measure name + time width
        QFontMetrics fmName(theme.nameFont());
        QFontMetrics fmTime(theme.timeFont());
        qreal nameTimeW = 0;
        if (!ml.isGrouped)
            nameTimeW = fmName.horizontalAdvance(ml.actorName) + PainterTheme::spacingSmall + fmTime.horizontalAdvance(ml.timeString);

        // Dynamic width: widest element, clamped to max
        qreal neededW = qMax(bodyIdealW, nameTimeW);
        if (!ml.replyToText.isEmpty()) {
            qreal quoteTextW = fmTime.horizontalAdvance(ml.replyToAuthor + ": " + ml.replyToText) + 20;
            neededW = qMax(neededW, qMin(quoteTextW, maxContentW));
        }
        if (ml.hasFile) neededW = maxContentW;
        if (!ml.reactions.isEmpty()) neededW = qMax(neededW, 100.0);
        if (!ml.isGrouped) {
            qreal groupedTimeW = fmTime.horizontalAdvance(ml.timeString);
            neededW = qMax(neededW, groupedTimeW);
        }

        qreal contentW = qBound(80.0, neededW + 2, maxContentW);

        // Re-layout body if width changed
        if (qAbs(contentW - maxContentW) > 1) {
            ml.bodyDoc = createBodyDoc(ml.bodyHtml, contentW, theme);
            bodyH = ml.bodyDoc->size().height();
        }

        // Name + time row (non-grouped)
        if (!ml.isGrouped) {
            int nameH = fmName.height();
            ml.nameRect = QRectF(contentX, y, contentW, nameH);
            int nameTextW = fmName.horizontalAdvance(ml.actorName);
            ml.timeRect = QRectF(
                contentX + nameTextW + PainterTheme::spacingSmall,
                y + (nameH - fmTime.height()) / 2.0,
                fmTime.horizontalAdvance(ml.timeString),
                fmTime.height()
            );
            y += nameH + 2;
        }

        // Reply quote
        if (!ml.replyToText.isEmpty()) {
            int quoteH = fmTime.height() * 2 + 12;
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

        ml.contentRight = contentX + contentW; // other messages: hover bar goes RIGHT of content
        y += 2; // bottom padding
    }

    ml.totalHeight = y - startY;
    return ml;
}

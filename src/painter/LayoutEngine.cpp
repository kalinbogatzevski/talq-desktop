#include "LayoutEngine.h"
#include "models/MessageListModel.h"
#include <QFontMetrics>
#include <QTextDocument>
#include <QRegularExpression>
#include <QtMath>

std::pair<qreal, qreal> LayoutEngine::fileRectSize(
    const QString &mime, qreal maxWidth, qreal maxThumbW,
    qreal maxThumbH, qreal imageAspectRatio)
{
    bool isImage = mime.startsWith(QLatin1String("image/"));
    if (isImage && imageAspectRatio > 0.01) {
        qreal w = qMin(maxWidth, maxThumbW);
        qreal h = qMin(w * imageAspectRatio, maxThumbH);
        return {w, h};
    }
    return {maxWidth, 44.0};
}

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

    // Skip empty messages — prevents empty gaps
    {
        QString strippedBody = ml.bodyHtml;
        static const QRegularExpression htmlTagRe(QStringLiteral("<[^>]*>"));
        strippedBody.remove(htmlTagRe);
        strippedBody = strippedBody.trimmed();
        bool hasRealFile = ml.hasFile && !ml.fileName.isEmpty();
        if (strippedBody.isEmpty() && !hasRealFile && ml.replyToText.isEmpty()) {
            ml.totalHeight = ml.showDateSep ? (y - startY) : 0;
            return ml;
        }
    }

    // ── Own message ──
    if (ml.isOwn) {

        // Avatar (non-grouped, same as other messages)
        if (!ml.isGrouped) {
            ml.avatarRect = QRectF(margin, y, PainterTheme::avatarSize, PainterTheme::avatarSize);
        }

        // Content positioned same as other messages (after avatar column)
        qreal contentX = margin + PainterTheme::avatarSize + PainterTheme::avatarGap;
        qreal maxContentW = bubbleMaxWidth - (PainterTheme::avatarSize + PainterTheme::avatarGap) - margin;

        ml.bodyDoc = createBodyDoc(ml.bodyHtml, maxContentW, theme);
        qreal bodyIdealW = ml.bodyDoc->idealWidth();
        qreal bodyH = ml.bodyDoc->size().height();

        QFontMetrics fmTime(theme.timeFont());
        qreal timeW = fmTime.horizontalAdvance(ml.timeString);

        qreal neededW = qMax(bodyIdealW, timeW);
        if (!ml.replyToText.isEmpty()) {
            qreal quoteTextW = fmTime.horizontalAdvance(ml.replyToAuthor + ": " + ml.replyToText) + 20;
            neededW = qMax(neededW, qMin(quoteTextW, maxContentW));
        }
        if (ml.hasFile) {
            auto [fw, fh] = fileRectSize(ml.fileMime, maxContentW, 200.0, 150.0, imageAspectRatio);
            neededW = qMax(neededW, fw);
        }
        if (!ml.reactions.isEmpty()) neededW = qMax(neededW, 100.0);

        qreal contentW = qBound(80.0, neededW + 2, maxContentW);

        if (qAbs(contentW - maxContentW) > 1) {
            ml.bodyDoc = createBodyDoc(ml.bodyHtml, contentW, theme);
            bodyH = ml.bodyDoc->size().height();
        }

        // Reply quote
        if (!ml.replyToText.isEmpty()) {
            int quoteH = fmTime.height() * 2 + 12;
            ml.quoteRect = QRectF(contentX, y, contentW, quoteH);
            y += quoteH + 4;
        }

        // File attachment area
        if (ml.hasFile) {
            auto [fileW, fileH] = fileRectSize(ml.fileMime, contentW, 200.0, 150.0, imageAspectRatio);
            ml.fileRect = QRectF(contentX, y, fileW, fileH);
            y += fileH + 4;
        }

        // Body text
        if (bodyH > 2) {
            ml.bodyRect = QRectF(contentX, y, contentW, bodyH);
            y += bodyH;
        }

        // Reactions
        if (!ml.reactions.isEmpty()) {
            ml.reactBarRect = QRectF(contentX, y + 2, contentW, 24);
            y += 28;
        }

        // Time below body
        ml.timeRect = QRectF(contentX, y + 2, timeW, fmTime.height());
        y += fmTime.height() + 4;

        // Bubble rect (computed from content for paint)
        ml.bubbleRect = QRectF(contentX, ml.avatarRect.isNull() ? ml.totalY : ml.totalY,
                                contentW, y - ml.totalY);
        ml.contentRight = contentX + contentW;

        y += 2;
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

        // Measure widths
        QFontMetrics fmName(theme.nameFont());
        QFontMetrics fmTime(theme.timeFont());
        qreal timeW = fmTime.horizontalAdvance(ml.timeString);

        // Dynamic width: widest element, clamped to max
        qreal neededW = qMax(bodyIdealW, timeW);
        if (!ml.replyToText.isEmpty()) {
            qreal quoteTextW = fmTime.horizontalAdvance(ml.replyToAuthor + ": " + ml.replyToText) + 20;
            neededW = qMax(neededW, qMin(quoteTextW, maxContentW));
        }
        if (ml.hasFile) {
            auto [fw, fh] = fileRectSize(ml.fileMime, maxContentW, 350.0, 250.0, imageAspectRatio);
            neededW = qMax(neededW, fw);
        }
        if (!ml.reactions.isEmpty()) neededW = qMax(neededW, 100.0);

        qreal contentW = qBound(80.0, neededW + 2, maxContentW);

        // Re-layout body if width changed
        if (qAbs(contentW - maxContentW) > 1) {
            ml.bodyDoc = createBodyDoc(ml.bodyHtml, contentW, theme);
            bodyH = ml.bodyDoc->size().height();
        }

        // Author name row (non-grouped) — time goes inside bubble, not here
        if (!ml.isGrouped) {
            int nameH = fmName.height();
            qreal nameW = qMin((qreal)fmName.horizontalAdvance(ml.actorName) + 4, maxContentW);
            ml.nameRect = QRectF(contentX, y, nameW, nameH);
            y += nameH + 6;
        }

        // Reply quote
        if (!ml.replyToText.isEmpty()) {
            int quoteH = fmTime.height() * 2 + 12;
            ml.quoteRect = QRectF(contentX, y, contentW, quoteH);
            y += quoteH + 4;
        }

        // File attachment area
        if (ml.hasFile) {
            auto [fileW, fileH] = fileRectSize(ml.fileMime, contentW, 350.0, 250.0, imageAspectRatio);
            ml.fileRect = QRectF(contentX, y, fileW, fileH);
            y += fileH + 4;
        }

        // Body text (skip if empty — e.g., file-only messages)
        if (bodyH > 2) {
            ml.bodyRect = QRectF(contentX, y, contentW, bodyH);
            y += bodyH;
        }

        // Reactions
        if (!ml.reactions.isEmpty()) {
            ml.reactBarRect = QRectF(contentX, y + 2, contentW, 24);
            y += 28;
        }

        // Time below body (always inside bubble)
        {
            QFontMetrics fmT(theme.timeFont());
            ml.timeRect = QRectF(
                contentX,
                y + 2,
                fmT.horizontalAdvance(ml.timeString),
                fmT.height()
            );
            y += fmT.height() + 4;
        }

        ml.contentRight = contentX + contentW; // other messages: hover bar goes RIGHT of content
        y += 2; // bottom padding
    }

    ml.totalHeight = y - startY;
    return ml;
}

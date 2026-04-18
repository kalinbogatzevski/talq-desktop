#include "LayoutEngine.h"
#include "core/EmojiData.h"
#include "models/MessageListModel.h"
#include <QFontMetrics>
#include <QTextBoundaryFinder>
#include <QTextDocument>
#include <QRegularExpression>
#include <QtMath>

static const QRegularExpression s_htmlTagRe(QStringLiteral("<[^>]*>"));

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
    ml.timeString          = model->data(idx, MessageListModel::TimeStringRole).toString();
    ml.lastEditTimestamp   = model->data(idx, MessageListModel::LastEditTimestampRole).toLongLong();
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
    ml.fileSize = model->data(idx, MessageListModel::FileSizeRole).toLongLong();

    ml.isOwn = (ml.actorId == myUserId);

    // Grouping: same author, within 300s, neither is system
    ml.isGrouped = !ml.isSystem && !prevIsSystem
        && !ml.actorId.isEmpty() && ml.actorId == prevActorId
        && prevTimestamp > 0 && qAbs(model->data(idx, MessageListModel::TimestampRole).toLongLong() - prevTimestamp) < 300;

    // If showing date separator, never group
    if (ml.showDateSep)
        ml.isGrouped = false;

    const qreal margin = PainterTheme::spacingNormal;  // 12px outer margin
    const qreal bubbleMaxWidth = width * 0.75;
    const qreal bubblePadX = 10;  // horizontal padding inside bubble
    const qreal bubblePadTop = 6;  // vertical padding top inside bubble
    const qreal bubblePadBottom = 6;  // vertical padding bottom inside bubble
    const qreal avatarCol = PainterTheme::avatarSize + PainterTheme::avatarGap;  // 44px
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
        strippedBody.remove(s_htmlTagRe);
        strippedBody = strippedBody.trimmed();
        bool hasRealFile = ml.hasFile && !ml.fileName.isEmpty();
        if (strippedBody.isEmpty() && !hasRealFile && ml.replyToText.isEmpty()) {
            ml.totalHeight = ml.showDateSep ? (y - startY) : 0;
            return ml;
        }
    }

    // ── Layout constants (shared by own and other messages) ──
    // Both own and other messages use the same left-aligned layout:
    //   [margin][avatar 36px][avatarGap 8px][bubblePadX][content][bubblePadX]
    //   Avatar column always reserved even for grouped messages.
    //   Bubble background spans from bubbleLeft to bubbleLeft + bubbleW.
    //   Content sits inside bubble with bubblePadX horizontal padding.

    qreal bubbleLeft = margin + avatarCol;          // where bubble background starts
    qreal contentX = bubbleLeft + bubblePadX;        // where text/content starts
    qreal maxContentW = qMax(40.0, bubbleMaxWidth - avatarCol - margin - 2 * bubblePadX);

    QFontMetrics fmName(theme.nameFont());
    QFontMetrics fmTime(theme.timeFont());
    qreal timeW = fmTime.horizontalAdvance(ml.timeString) + (ml.isOwn ? 20 : 8);

    // ── Avatar (non-grouped) ──
    if (!ml.isGrouped) {
        ml.avatarRect = QRectF(margin, y, PainterTheme::avatarSize, PainterTheme::avatarSize);
    }

    // ── Author name (non-grouped, above bubble) ──
    if (!ml.isGrouped && !ml.isOwn) {
        int nameH = fmName.height();
        qreal nameW = qMin((qreal)fmName.horizontalAdvance(ml.actorName) + 4, maxContentW + 2 * bubblePadX);
        ml.nameRect = QRectF(bubbleLeft, y, nameW, nameH);
        y += nameH + 4;
    }

    // ── Measure body text ──
    ml.bodyDoc = createBodyDoc(ml.bodyHtml, maxContentW, theme);
    qreal bodyIdealW = ml.bodyDoc->idealWidth();
    qreal bodyH = ml.bodyDoc->size().height();

    // ── Determine content width (shrink-to-fit) ──
    qreal neededW = qMax(bodyIdealW, timeW);
    if (!ml.replyToText.isEmpty()) {
        qreal quoteTextW = fmTime.horizontalAdvance(ml.replyToAuthor + ": " + ml.replyToText) + 20;
        neededW = qMax(neededW, qMin(quoteTextW, maxContentW));
    }
    if (ml.hasFile) {
        qreal maxThumbW = ml.isOwn ? 200.0 : 350.0;
        qreal maxThumbH = ml.isOwn ? 150.0 : 250.0;
        auto [fw, fh] = fileRectSize(ml.fileMime, maxContentW, maxThumbW, maxThumbH, imageAspectRatio);
        neededW = qMax(neededW, fw);
    }
    if (!ml.reactions.isEmpty()) neededW = qMax(neededW, 100.0);

    qreal contentW = qBound(80.0, neededW + 2, maxContentW);

    // Re-layout body if width changed
    if (qAbs(contentW - maxContentW) > 1) {
        ml.bodyDoc = createBodyDoc(ml.bodyHtml, contentW, theme);
        bodyH = ml.bodyDoc->size().height();
    }

    // ── Detect emoji cluster runs (read-only; no document mutation) ──
    if (ml.bodyDoc) {
        QString plain = ml.bodyDoc->toPlainText();
        QTextBoundaryFinder bf(QTextBoundaryFinder::Grapheme, plain);
        int start = 0;
        bf.setPosition(0);
        int next = bf.toNextBoundary();
        while (next != -1) {
            int end = next;
            QString cluster = plain.mid(start, end - start);
            if (EmojiData::isEmojiCluster(cluster)) {
                ml.emojiRuns.append({start, cluster});
            }
            start = end;
            next = bf.toNextBoundary();
        }
    }

    // ── Bubble content starts here (with top padding) ──
    qreal bubbleTopY = y;
    y += bubblePadTop;

    // Reply quote
    if (!ml.replyToText.isEmpty()) {
        int quoteH = fmTime.height() * 2 + 12;
        ml.quoteRect = QRectF(contentX, y, contentW, quoteH);
        y += quoteH + 4;
    }

    // File attachment area
    if (ml.hasFile) {
        qreal maxThumbW = ml.isOwn ? 200.0 : 350.0;
        qreal maxThumbH = ml.isOwn ? 150.0 : 250.0;
        auto [fileW, fileH] = fileRectSize(ml.fileMime, contentW, maxThumbW, maxThumbH, imageAspectRatio);
        ml.fileRect = QRectF(contentX, y, fileW, fileH);
        y += fileH + 4;
    }

    // Body text
    if (bodyH > 2) {
        ml.bodyRect = QRectF(contentX, y, contentW, bodyH);
        y += bodyH + 2;
    }

    // Reactions
    if (!ml.reactions.isEmpty()) {
        ml.reactBarRect = QRectF(contentX, y + 2, contentW, 24);
        y += 28;
    }

    // Timestamp (right-aligned inside bubble)
    ml.timeRect = QRectF(contentX + contentW - timeW, y + 2, timeW, fmTime.height());
    y += fmTime.height() + 4;

    // ── Bottom padding ──
    y += bubblePadBottom;

    // ── Bubble rect (for painting) ──
    qreal bubbleW = contentW + 2 * bubblePadX;
    ml.bubbleRect = QRectF(bubbleLeft, bubbleTopY, bubbleW, y - bubbleTopY);
    ml.contentRight = bubbleLeft + bubbleW;

    y += 2;  // inter-message gap

    ml.totalHeight = y - startY;
    return ml;
}

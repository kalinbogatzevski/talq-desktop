#pragma once

#include "MessageLayout.h"
#include "PainterTheme.h"
#include <QAbstractListModel>

/**
 * Computes MessageLayout structs from model data.
 * Handles text measurement via QTextDocument, geometry for avatars,
 * names, bubbles, timestamps, reply quotes, and date separators.
 */
class LayoutEngine
{
public:
    /**
     * Compute layout for a single message.
     *
     * @param model       The message list model
     * @param modelRow    Row index in the model (newest-first)
     * @param width       Available width for the entire chat area
     * @param theme       Current theme for colors/fonts/spacing
     * @param startY      Y position where this message starts (document space)
     * @param myUserId    Current user's ID (to determine own vs other)
     * @param prevActorId actorId of the previous message (for grouping, empty if none)
     * @param prevTimestamp timestamp of the previous message (for grouping, 0 if none)
     * @param prevIsSystem whether the previous message was a system message
     * @return Fully computed MessageLayout
     */
    static MessageLayout computeLayout(
        QAbstractListModel *model,
        int modelRow,
        qreal width,
        const PainterTheme &theme,
        qreal startY,
        const QString &myUserId,
        const QString &prevActorId,
        qint64 prevTimestamp,
        bool prevIsSystem,
        qreal imageAspectRatio = 0.75  // height/width, 0.75 default until image loads
    );

private:
    static std::shared_ptr<QTextDocument> createBodyDoc(
        const QString &html, qreal maxWidth, const PainterTheme &theme);
};

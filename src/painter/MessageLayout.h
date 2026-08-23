#pragma once

#include <QRectF>
#include <QString>
#include <QTextDocument>
#include <QVector>
#include <memory>

/**
 * Hit region for future click handling (Phase 7).
 */
struct HitRegion {
    enum Type { None, Avatar, Name, Body, Time, Quote, Reaction, Link };
    Type type = None;
    QRectF rect;
    QString data;  // URL for links, emoji for reactions, etc.
};

/**
 * Location of one emoji grapheme cluster inside a message's body document.
 * LayoutEngine scans the document at layout time and records the position
 * without mutating the text; ChatPainter overlays a Twemoji pixmap at that
 * position during paintEvent. docPosition is valid as long as bodyDoc is
 * not re-laid-out after the runs are populated.
 */
struct EmojiRun {
    int docPosition;       // char offset in bodyDoc->toPlainText()
    QString codepoints;    // the full grapheme cluster
};

/**
 * Pre-computed layout for a single message, produced by LayoutEngine.
 * All Y coordinates are absolute (document-space), not viewport-relative.
 */
struct MessageLayout
{
    // ── Identity ──
    int messageId = 0;
    int modelRow = 0;       // row in the model (newest-first)

    // ── Flags ──
    bool isOwn = false;
    bool isGrouped = false;
    bool isSystem = false;
    bool showDateSep = false;

    // ── Text data (copied from model for painting) ──
    QString actorName;
    QString actorId;
    QString bodyHtml;       // message text (HTML)
    QString timeString;
    qint64 timestamp = 0;
    qint64 lastEditTimestamp = 0;
    QString dateString;
    QString sendStatus;
    bool isRead = false;

    // ── Reply quote data ──
    QString replyToAuthor;
    QString replyToText;

    // ── Reactions data ──
    QString reactions;

    // ── File attachment data ──
    bool hasFile = false;
    int fileId = 0;
    QString fileName;
    // Path in the user's Files, so a forward can re-share the attachment.
    QString filePath;
    // Emoji the CURRENT user has reacted with, so their own pills can be
    // marked. Without it a pill cannot say whether you are in the count.
    QStringList reactionsSelf;
    // Poll card. pollId > 0 means this row renders a poll instead of prose.
    int pollId = 0;
    QString pollQuestion;
    QRectF pollRect;
    QString fileMime;
    qint64 fileSize = 0;

    // ── Geometry (absolute Y in document space) ──
    qreal totalY = 0;
    qreal totalHeight = 0;

    // Date separator (if showDateSep)
    QRectF dateSepRect;     // the pill background
    QRectF dateSepTextRect; // text inside the pill

    bool showUnreadSep = false;
    QRectF unreadSepRect;   // horizontal strip where the "New messages" pill sits

    // Avatar (others, non-grouped only)
    QRectF avatarRect;

    // Author name (others, non-grouped only)
    QRectF nameRect;

    // Message body (text)
    QRectF bodyRect;

    // Bubble rect (own messages get a rounded-rect background)
    QRectF bubbleRect;
    qreal contentRight = 0;  // right edge of actual content (for hover bar positioning)

    // Timestamp
    QRectF timeRect;

    // Reply quote (if present) — painted but not interactive yet
    QRectF quoteRect;

    // File attachment area
    QRectF fileRect;        // image preview or file pill rect

    // Edge-to-edge image: a pure-image message (no caption / reply / reactions)
    // where the image IS the bubble — drawn flush to the bubble edges, rounded
    // to the bubble radius, with the timestamp floated over the bottom-right on
    // a scrim. Set by LayoutEngine; consumed by ChatPainter.
    bool imageBubble = false;

    // Reactions bar
    QRectF reactBarRect;

    // ── Cached rich-text document for body ──
    std::shared_ptr<QTextDocument> bodyDoc;

    // ── Emoji runs (populated by LayoutEngine, consumed by ChatPainter) ──
    QVector<EmojiRun> emojiRuns;

    // ── Hit regions (populated for future phases) ──
    // Kept minimal for now
};

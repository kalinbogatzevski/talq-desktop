#pragma once

#include <QRectF>
#include <QString>
#include <QTextDocument>
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
    QString dateString;
    QString sendStatus;
    bool isRead = false;

    // ── Reply quote data ──
    QString replyToAuthor;
    QString replyToText;

    // ── Reactions data ──
    QString reactions;

    // ── Geometry (absolute Y in document space) ──
    qreal totalY = 0;
    qreal totalHeight = 0;

    // Date separator (if showDateSep)
    QRectF dateSepRect;     // the pill background
    QRectF dateSepTextRect; // text inside the pill

    // Avatar (others, non-grouped only)
    QRectF avatarRect;

    // Author name (others, non-grouped only)
    QRectF nameRect;

    // Message body (text)
    QRectF bodyRect;

    // Bubble rect (own messages get a rounded-rect background)
    QRectF bubbleRect;

    // Timestamp
    QRectF timeRect;

    // Reply quote (if present) — painted but not interactive yet
    QRectF quoteRect;

    // Reactions bar — space reserved, not painted in Phase 3
    QRectF reactBarRect;

    // ── Cached rich-text document for body ──
    std::shared_ptr<QTextDocument> bodyDoc;

    // ── Hit regions (populated for future phases) ──
    // Kept minimal for now
};

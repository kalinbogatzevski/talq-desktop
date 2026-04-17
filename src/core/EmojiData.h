#pragma once

#include <QString>
#include <QStringList>
#include <QVector>
#include <QHash>
#include <QPixmap>

namespace EmojiData {

enum class Category {
    Smileys,
    People,
    Animals,
    Food,
    Activities,
    Travel,
    Objects,
    Symbols,
    Flags,
};

struct EmojiEntry {
    QString codepoints;         // e.g. "\xF0\x9F\x98\x80" for 😀
    Category category;
    QString name;               // e.g. "grinning face"
    QStringList shortcodes;     // e.g. {":grinning:"}
    QStringList shortForms;     // e.g. {":)", ":-)"} — usually empty
    QStringList skinToneVariants; // codepoint strings for each tone, or empty
    QStringList keywords;
};

// Loaded once at startup from the generated array.
void initialize();

const QVector<EmojiEntry> &allEntries();
const EmojiEntry *findByShortcode(const QString &shortcode);
const EmojiEntry *findByShortForm(const QString &sf);
QVector<const EmojiEntry*> search(const QString &query);
QVector<const EmojiEntry*> inCategory(Category c);

QPixmap pixmapFor(const QString &codepoints, int sizePx);

// Recents — persisted in QSettings under "emoji/recent".
QVector<const EmojiEntry*> recent();
void pushRecent(const EmojiEntry *e);

} // namespace EmojiData

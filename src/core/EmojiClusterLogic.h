#pragma once

// Does a grapheme cluster of a message get a Twemoji PICTURE drawn over it, or is
// it text?
//
// TalQ draws emoji itself, from Twemoji images, instead of leaving them to the
// system font (LayoutEngine for a message body, EmojiTextRenderer for the sidebar
// and previews). The test is a range heuristic: a cluster with any code point in a
// known emoji range gets the picture.
//
// One thing the range cannot know, and the Unicode standard does say: U+FE0E
// (VARIATION SELECTOR-15, "text presentation") after a character asks for the TEXT
// form. A cluster that carries it is text, whatever its first character is.
// Without this a task-list box could not be drawn as text: U+2611 (BALLOT BOX WITH
// CHECK) is in the Dingbats block, so every ticked box got Twemoji's blue emoji
// while the empty box next to it (U+2610, which Twemoji has no picture for) was a
// plain outline. Anyone typing U+2611 U+FE0E now gets the text form too.
//
// U+FE0F (VARIATION SELECTOR-16, "emoji presentation") needs no rule: such a
// cluster is in the range already and stays an emoji.
//
// Pure Qt-Core. Test: emoji-cluster-logic-test.

#include <QChar>
#include <QString>

namespace talq::emoji {

inline bool isEmojiCluster(const QString &cluster)
{
    // Text presentation was asked for, explicitly.
    if (cluster.contains(QChar(0xFE0E)))
        return false;

    for (int i = 0; i < cluster.size(); ) {
        uint cp = cluster[i].unicode();
        if (QChar::isHighSurrogate(cp) && i + 1 < cluster.size()) {
            cp = QChar::surrogateToUcs4(cluster[i], cluster[i + 1]);
            i += 2;
        } else ++i;
        if ((cp >= 0x1F300 && cp <= 0x1FAFF) ||   // Misc + Supplemental Symbols
            (cp >= 0x2600  && cp <= 0x27BF)  ||   // Misc Symbols + Dingbats
            (cp >= 0x1F1E6 && cp <= 0x1F1FF) ||   // Regional Indicators (flag halves)
            cp == 0x00A9 || cp == 0x00AE)         // © ®
            return true;
    }
    return false;
}

} // namespace talq::emoji

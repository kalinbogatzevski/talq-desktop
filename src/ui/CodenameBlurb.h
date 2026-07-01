#pragma once
#include <QString>
#include <QObject>

// Per-codename one-line explanation, shared by the MainWindow codename pill
// tooltip, the Settings version-line tooltip, and the Settings About credit so the
// three can never drift. Keyed on TALQ_VERSION_NAME; unknown/future names fall back
// to a neutral label rather than a wrong story.
inline QString codenameBlurb(const QString &verName)
{
    if (verName.isEmpty()) return QString();
    if (verName == QStringLiteral("Enyov Day"))
        return QObject::tr("Codename \"%1\" — Enyovden (Еньовден), the "
            "Bulgarian Midsummer, 24 June: the summer solstice, when the sun is at "
            "its peak and begins its turn toward winter, and the feast of St John "
            "the Baptist. Above all the herbalists' day — healing herbs gathered "
            "at dawn are at their most potent (the legendary \"77 and a half\": 77 "
            "for 77 ailments, and a half for the one only a few healers know). "
            "Fitting for a release about healing what was broken.").arg(verName);
    if (verName == QStringLiteral("Bafana Bafana"))
        return QObject::tr("Codename \"%1\" — \"the boys\", the nickname of South "
            "Africa's national football team. On 24 June 2026 they won 1–0 (a Thapelo "
            "Maseko strike) to finish their group and reach a World Cup knockout round "
            "for the first time in their history — through to the last 32 in their "
            "fourth finals, having never before made it out of the group. A nod home: "
            "the team and the company that builds TalQ both come from South "
            "Africa.").arg(verName);
    if (verName == QStringLiteral("Deep Thought"))
        return QObject::tr("Codename \"%1\" — the supercomputer from The "
            "Hitchhiker's Guide to the Galaxy that computed 42, the Answer to "
            "Life, the Universe and Everything (a nod to version 0.42).").arg(verName);
    if (verName == QStringLiteral("Magrathea"))
        return QObject::tr("Codename \"%1\" — the legendary planet-building world "
            "from The Hitchhiker's Guide to the Galaxy, where bespoke luxury "
            "planets are made to order.").arg(verName);
    if (verName == QStringLiteral("Slartibartfast"))
        return QObject::tr("Codename \"%1\" — the Hitchhiker's Guide planetary "
            "coastline designer who won an award for the fjords of Norway and "
            "liked to sign his name in the crinkly bits; a maker who sweats the "
            "fine detail.").arg(verName);
    if (verName == QStringLiteral("Botev"))
        return QObject::tr("Codename \"%1\" — Hristo Botev, the poet-revolutionary "
            "honoured on Bulgaria's Heroes' Day (2 June).").arg(verName);
    if (verName == QStringLiteral("Margaritka"))
        return QObject::tr("Codename \"%1\" — the daisy (margaritka), for "
            "Bulgaria's Children's Day (1 June).").arg(verName);
    if (verName == QStringLiteral("Aprilsko Vastanie")
        || verName == QStringLiteral("Panagyurishte")
        || verName == QStringLiteral("Koprivshtitsa"))
        return QObject::tr("Codename \"%1\" — Bulgaria's April Uprising of 1876, "
            "150th anniversary (2026).").arg(verName);
    return QObject::tr("Codename \"%1\".").arg(verName);
}

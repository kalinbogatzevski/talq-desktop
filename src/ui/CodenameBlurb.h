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
    if (verName == QStringLiteral("July Morning"))
        return QObject::tr("Codename \"%1\" — the Bulgarian tradition of driving "
            "east to the Black Sea cliffs on the night of 30 June and staying "
            "awake to greet the first sunrise of 1 July together; a 1980s-"
            "counterculture ritual carried by Uriah Heep's 1971 song, sung at the "
            "clifftop of Kamen Bryag. A name about renewal and a fresh start at "
            "first light — for the release that made calls finally feel clean "
            "(echo cancellation on speakers, and a wave of call-reliability "
            "fixes).").arg(verName);
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
    if (verName == QStringLiteral("Slivnitsa"))
        return QObject::tr("Codename \"%1\" — Сливница, "
            "5–7 November 1885, the battle that defended the Unification two "
            "months after it was declared. Serbia attacked expecting a walkover: "
            "Russia had recalled every senior Russian officer from the Bulgarian "
            "army in protest at the Unification, so the units that met them were "
            "led by captains and lieutenants — it is remembered as the war of "
            "the captains against the generals. The army was massed on the "
            "Ottoman frontier in the south, the wrong end of the country; it "
            "marched the length of Bulgaria to the ridge at Slivnitsa, some "
            "arriving straight into the line. They held for three days under "
            "Danail Nikolaev, then counter-attacked, took Pirot, and were still "
            "advancing when Austria-Hungary intervened to stop them short of "
            "Belgrade. The Treaty of Bucharest moved not one border — which "
            "was the whole point: the Unification stood. A name over the release "
            "that makes the previous line's work stable, and over a run of "
            "fixes for things that had been quietly wrong for a long time.").arg(verName);
    if (verName == QStringLiteral("Saedinenie"))
        return QObject::tr("Codename \"%1\" — Съединение, the Unification of "
            "Bulgaria, 6 September 1885. The Treaty of Berlin had cut the country "
            "in two seven years earlier: the Principality north of the Balkan "
            "range, and Eastern Rumelia south of it, left under Ottoman "
            "suzerainty. The Bulgarian Secret Central Revolutionary Committee, "
            "led by Zahari Stoyanov, spent 1885 preparing to undo that. It began "
            "in the village of Golyamo Konare — renamed Saedinenie afterwards, "
            "and still called that today. On 6 September the militia entered "
            "Plovdiv under Danail Nikolaev, deposed Governor-General Gavril "
            "Krastevich without a shot fired, and proclaimed the two halves one "
            "country; Prince Alexander I accepted it rather than disown them. "
            "Serbia declared war that November expecting an easy border "
            "adjustment, and lost at Slivnitsa to an army whose senior Russian "
            "officers had been withdrawn — it is remembered as the war of the "
            "captains against the generals. \"Съединението прави силата\", "
            "unity makes strength, has been on the coat of arms ever since. A "
            "name over the release line that put things back together: a reply "
            "now leads back to what it answers, and editing a message no longer "
            "severs it from the way it was written.").arg(verName);
    if (verName == QStringLiteral("Blue Fiesta Week 4 Re-match"))
        return QObject::tr("Codename \"%1\" — the return leg, away in Athens on "
            "25 August 2026, of the play-off tie that \"Blue Fiesta Week 4\" was "
            "named for: the last gate before the Champions League proper, for "
            "PFC Levski Sofia, \"Sinite\" (the Blues), the maintainer's club. "
            "The name was deliberately reused rather than replaced, because the "
            "fixture was the same one — and it then carried three release lines, "
            "which is unusual and also the point: 0.65.x brought the Talk 24 work "
            "(conversation tags, presets and voice rooms, breakout rooms, topics "
            "you follow across every conversation, poll drafts), 0.67.x added the "
            "caller screen-pop that ties the phone on your desk to the customer "
            "on your screen, and 0.68.x promoted all of it to stable. A long tie, "
            "and a long line.").arg(verName);
    if (verName == QStringLiteral("Blue Fiesta Week 4"))
        return QObject::tr("Codename \"%1\" — week four of the blue celebration "
            "for PFC Levski Sofia, \"Sinite\" (the Blues), the maintainer's club: "
            "the PLAY-OFF round of the 2026–27 UEFA Champions League, the last "
            "gate before the league phase, reached by winning through the "
            "qualifying rounds that \"Blue Fiesta\" and weeks two and three were "
            "named for. Twenty years earlier, in 2006–07, Levski became the first "
            "Bulgarian club ever to reach the Champions League group stage, drawn "
            "with Barcelona, Chelsea and Werder Bremen. A name over the release "
            "that stopped guessing about echo and went and measured it — proving "
            "the cancellation does its job, and closing the ways it could quietly "
            "stop doing it — and then over the pass that made every theme meet "
            "the contrast standard rather than nearly meet it.").arg(verName);
    if (verName == QStringLiteral("Blue Fiesta Week 3"))
        return QObject::tr("Codename \"%1\" — week three of the blue celebration "
            "for PFC Levski Sofia, \"Sinite\" (the Blues), the maintainer's club: "
            "the THIRD qualifying round of the 2026–27 UEFA Champions League, and a "
            "1–0 win over Kazakh champions Kairat at a packed Georgi Asparuhov on "
            "4 August — Serginho settling it in the second minute of stoppage time, "
            "a low shot deflected past Anarbekov with the game all but gone. The "
            "return is played not in Almaty but in Turkistan, 830 km west, after "
            "UEFA allowed Kairat to move it when their Central Stadium was given "
            "over to a concert. Twenty years earlier, in 2006–07, Levski became the "
            "first Bulgarian club ever to reach the Champions League group stage, "
            "drawn with Barcelona, Chelsea and Werder Bremen. A name over the "
            "release that went looking for what leaks: every video hand-off now "
            "bounded, what teardown forgot now freed, and the places the interface "
            "rendered wrong put right.").arg(verName);
    if (verName == QStringLiteral("Blue Fiesta Week 2"))
        return QObject::tr("Codename \"%1\" — week two of the blue celebration for "
            "PFC Levski Sofia, \"Sinite\" (the Blues), the maintainer's club: the "
            "SECOND qualifying round of the 2026–27 UEFA Champions League, reached "
            "as Bulgarian champions by putting out Bosnian champions Borac Banja "
            "Luka 5–1 on aggregate, and drawn against Romanian champions "
            "Universitatea Craiova — Kristian Dimitrov's eighth-minute goal "
            "settling the first leg in Sofia. Twenty years earlier, in 2006–07, "
            "Levski became the first Bulgarian club ever to reach the Champions "
            "League group stage, drawn with Barcelona, Chelsea and Werder Bremen. "
            "A name about momentum carried forward, over the release line that "
            "made video lighter on older and lower-powered computers and steadied "
            "screen sharing.").arg(verName);
    if (verName == QStringLiteral("Blue Fiesta"))
        return QObject::tr("Codename \"%1\" — for PFC Levski Sofia, \"Sinite\" (the "
            "Blues), the maintainer's club, and the blue celebration of its fans as "
            "Levski opens a new UEFA Champions League qualifying campaign. In 2006–07 "
            "Levski became the first Bulgarian club ever to reach the Champions "
            "League group stage, drawn with Barcelona, Chelsea and Werder Bremen. A "
            "fitting flag over the release that hardened calls and screen "
            "sharing.").arg(verName);
    return QObject::tr("Codename \"%1\".").arg(verName);
}

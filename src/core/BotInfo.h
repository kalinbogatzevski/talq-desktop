#pragma once

#include <QString>

// NC Talk bot framework — a bot is a server-wide registration with a
// webhook URL that can be enabled/disabled per conversation. Returned by
// /apps/spreed/api/v1/bot/{token} (per-room) and /api/v1/bot/admin (server).
struct BotInfo {
    int id = 0;
    QString name;
    QString description;
    // state: 0 = no setup yet for this room, 1 = enabled, 2 = disabled,
    //        3 = no-setup with error. Only meaningful from the per-room
    //        endpoint; from /admin it's always 0.
    int state = 0;
    // Bitmask: 1=webhook, 2=response, 4=event, 8=reaction
    int features = 0;
    QString errorMessage;  // populated when state == 3

    bool isEnabled() const { return state == 1; }
};

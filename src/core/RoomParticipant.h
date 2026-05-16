#pragma once

#include <QString>

struct RoomParticipant {
    // Spreed "participantType" values:
    //   1 = owner, 2 = moderator, 3 = user, 4 = guest,
    //   5 = user-self-joined, 6 = guest-moderator
    enum Role { Owner = 1, Moderator = 2, User = 3, Guest = 4,
                UserSelfJoined = 5, GuestModerator = 6 };

    QString userId;         // "" for guests; for users this is NC user id
    QString displayName;
    int     participantType = User;
    qint64  attendeeId = 0; // stable per-room id used by participant endpoints
    QString status;         // online / away / dnd / offline / ""
};

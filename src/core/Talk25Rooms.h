#pragma once
// Pure, Qt-free decisions for the Talk 25 room kinds: channels, announcements,
// classified conversations and preserved conversations. Callers pass
// primitives, so every rule below is unit-testable without a server or a
// widget (mirrors ServerCapabilities.h / ChatSyncLogic.h).
//
// Everything here was read out of nextcloud/spreed v25.0.7 (Nextcloud 35), not
// out of its docs, which carry no API text for these features:
//   lib/RoomAttributes.php, lib/Participant.php, lib/Service/RoomFormatter.php,
//   lib/Controller/RoomController.php, lib/Listener/ClassifiedRoomAutoDeleteListener.php,
//   lib/BackgroundJob/ExpireObjectRooms.php, lib/Manager.php.
#include <string>

#include "core/ServerCapabilities.h"

namespace talq {

// Talk's PERMISSIONS_CHAT bit (lib/Model/Attendee.php). `permissions` on a room
// is the EFFECTIVE mask for the current user: a moderator already gets the
// maximum, so a plain bit test is the whole check.
constexpr int kPermissionChat = 128;

inline bool isPreservedRoom(int attributes)    { return (attributes & AttributePreserve) != 0; }
inline bool isClassifiedRoom(int attributes)   { return (attributes & AttributeClassified) != 0; }
inline bool isChannelRoom(int attributes)      { return (attributes & AttributeChannel) != 0; }
inline bool isAnnouncementRoom(int attributes) { return (attributes & AttributeAnnouncement) != 0; }

// The one label a room gets in the list and the header. An announcement always
// carries the channel bit as well, so it is tested first. Classified is listed
// first of all: it is the kind that can cost people their conversation.
enum class RoomKind { Plain, Classified, Announcement, Channel };

inline RoomKind roomKind(int attributes)
{
    if (isClassifiedRoom(attributes))   return RoomKind::Classified;
    if (isAnnouncementRoom(attributes)) return RoomKind::Announcement;
    if (isChannelRoom(attributes))      return RoomKind::Channel;
    return RoomKind::Plain;
}

// Why the composer cannot post, if it cannot. Today the server is the only
// judge (a POST comes back 403), and the user types a message before finding
// out. `hasPermissions` is false when the server did not send the field (an
// older Talk), which must read as "allowed" so nothing changes there.
enum class PostBlock { None, ReadOnly, ChannelModeratorsOnly, NoPermission };

inline PostBlock postBlock(int attributes, bool readOnly, bool hasPermissions, int permissions)
{
    if (readOnly)
        return PostBlock::ReadOnly;
    if (hasPermissions && (permissions & kPermissionChat) == 0)
        return isChannelRoom(attributes) ? PostBlock::ChannelModeratorsOnly
                                         : PostBlock::NoPermission;
    return PostBlock::None;
}

// Leave / delete. `canLeave` / `canDelete` are the server's own
// `canLeaveConversation` / `canDeleteConversation`; an absent field parses as
// TRUE (see Conversation::fromJson), so this only ever REMOVES an action the
// server has said it will refuse and never adds one.
enum class LeaveBlock { None, Announcement, Refused };

inline LeaveBlock leaveBlock(int attributes, bool canLeave)
{
    if (canLeave) return LeaveBlock::None;
    return isAnnouncementRoom(attributes) ? LeaveBlock::Announcement : LeaveBlock::Refused;
}

enum class DeleteBlock { None, Preserved, Refused };

inline DeleteBlock deleteBlock(int attributes, bool canDelete)
{
    if (isPreservedRoom(attributes)) return DeleteBlock::Preserved;
    return canDelete ? DeleteBlock::None : DeleteBlock::Refused;
}

// ---------------------------------------------------------------------------
// Classified auto-deletion. THIS CAN COST PEOPLE THEIR CONVERSATION.
//
// When a call ENDS in a classified room, Talk marks the room with object type
// "classified" (ClassifiedRoomAutoDeleteListener). A background job then
// deletes every such room whose `last_activity` is at or before
// now - retention (ExpireObjectRooms / Manager::getExpiringRoomsForObjectType).
//
// Two details that are easy to get wrong, both verified in the v25.0.7 source:
//  - The clock is the room's LAST ACTIVITY, not the time of the call. The
//    capability text ("deleted after a call happened") is loose: a new message
//    pushes the deletion back, and the object id (a Unix time) is only a marker.
//  - A moderator keeps the room with DELETE /room/{token}/object, which turns
//    the object type into "classified_persist". It is then never swept and
//    never re-queued by a later call.
//
// Returns the Unix time from which the server may delete the room, or 0 when
// none is queued: not marked "classified", retention disabled (0), or no
// activity time known. The job runs periodically, so this is the EARLIEST
// moment, to be shown as "from about", never as an exact time.
// ---------------------------------------------------------------------------
inline long long classifiedDeleteAt(const std::string &objectType, long long lastActivity,
                                    long long retentionSeconds)
{
    if (objectType != "classified") return 0;
    if (retentionSeconds <= 0 || lastActivity <= 0) return 0;
    return lastActivity + retentionSeconds;
}

} // namespace talq

#pragma once

// Why the server would not schedule a message, as a closed set.
//
// The reasons are the ones ChatController::scheduleMessage (Talk 24) answers with, in
// ocs.data.error: 400 message | send-at | reply-to, 403 reply-to, 404 actor, 413 message. The
// words belong to the model, which translates them; this only decides WHICH reason applies, so
// the mapping is testable without Qt and an unrecognised answer lands on Unknown (the generic
// "(HTTP N)" text) instead of being shown raw.

#include <string>

namespace talq {

enum class ScheduleRefusal {
    Unknown,           // unrecognised, or no usable reason came back
    NoConnection,      // never reached the server (status 0)
    PastTime,          // the send time is not in the future (send-at)
    EmptyMessage,      // there was no text (message, 400)
    TooLong,           // over the server's limit (413)
    ReplyGone,         // the message replied to, or the topic, is no longer there (reply-to, 400)
    ReplyForbidden,    // may not reply to that message from here (reply-to, 403)
    NotAMember,        // guests and self-joined users cannot schedule (actor, 404)
};

inline ScheduleRefusal classifyScheduleFailure(int status, const std::string &error)
{
    if (status <= 0)
        return ScheduleRefusal::NoConnection;
    if (status == 413)
        return ScheduleRefusal::TooLong;
    if (error == "send-at")
        return ScheduleRefusal::PastTime;
    if (error == "message")
        return ScheduleRefusal::EmptyMessage;
    if (error == "reply-to")
        return status == 403 ? ScheduleRefusal::ReplyForbidden : ScheduleRefusal::ReplyGone;
    if (error == "actor")
        return ScheduleRefusal::NotAMember;
    return ScheduleRefusal::Unknown;
}

} // namespace talq

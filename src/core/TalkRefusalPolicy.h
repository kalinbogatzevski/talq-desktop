#pragma once

// Why Talk refused a request, as a closed set, for the failure branches that can say more than
// "Failed to ..." or "(HTTP N)".
//
// Talk answers a refusal with an HTTP status and, from the controller itself, a machine word in
// ocs.data.error (ApiClient hands that `data` to the callback with ok=false; OcsErrorBody.h). The
// SAME word means different things on different routes and statuses: "message" is an empty text
// at 400, a missing message at 404 and a system message at 405; "poll" is an ended poll at 400 and
// a missing one at 404. So a code is only ever read together with the request and the status.
// A refusal from Talk's middleware (read-only conversation, missing permission, lobby, not a
// moderator) carries NO code, only the status; it is mapped only where the route has a single
// such source (pin: its one 403 is "not a moderator").
//
// Only reasons a user can understand or act on are listed; everything else, a code this build has
// never heard of included, is Unknown, which the caller words as its generic text. The words
// belong to the caller (it translates them and knows the context): a code is never shown raw.
//
// Verified against spreed v25.0.7: ChatController (editMessage, deleteMessage, pinMessage,
// editScheduledMessage, deleteScheduleMessage, setReminder), PollController (createPoll, votePoll,
// closePoll), PollService::votePoll, Poll::setQuestion/setOptions, InjectionMiddleware.

#include <string>

namespace talq {

enum class TalkRequest {
    EditMessage,       // PUT    chat/{token}/{id}
    DeleteMessage,     // DELETE chat/{token}/{id}
    PinMessage,        // POST   chat/{token}/{id}/pin
    EditScheduled,     // POST   chat/{token}/schedule/{id}
    CancelScheduled,   // DELETE chat/{token}/schedule/{id}
    CreatePoll,        // POST   poll/{token}
    VotePoll,          // POST   poll/{token}/{id}
    ClosePoll,         // DELETE poll/{token}/{id}
    SetReminder,       // POST   chat/{token}/{id}/reminder
};

enum class TalkRefusal {
    Unknown,              // unrecognised, or no usable reason came back: the caller's generic text
    NoConnection,         // never reached the server (status 0)
    MessageGone,          // the message is not there (404 message: edit, pin, reminder)
    NotYourMessage,       // not the author, and not a moderator who may (403 permission: edit, delete)
    WrongKind,            // a system message, or a shared object (405 message: edit, delete; 400 message: pin)
    TooOldToEdit,         // edits are taken for 24 hours after sending (400 age: edit)
    EmptyText,            // the new text is empty (400 message: edit, scheduled edit)
    TooLong,              // over the server's limit (413: edit, scheduled edit)
    NotModerator,         // only moderators may do this (403 without a code: pin)
    ScheduledGone,        // the scheduled message was sent or cancelled already (404 message)
    PastTime,             // the new send time is not in the future (400 send-at: scheduled edit)
    NotAMember,           // guests and self-joined users have no scheduled messages (404 actor)
    PollEnded,            // the poll is closed (400 poll: vote, close)
    PollGone,             // the poll is not there, or is a draft this user may not see (404 poll)
    TooManyChoices,       // more answers picked than the poll allows (400 options: vote)
    PollNotYours,         // only the poll's author or a moderator may end it (403 poll: close)
    DraftNeedsModerator,  // only moderators may save a poll as a draft (400 draft: create)
    NotAGroup,            // polls exist in group and public conversations only (400 room: create)
    QuestionTooLong,      // over 32000 bytes; the composer already refuses an empty one (400 question)
    AnswersTooLong,       // over 60000 bytes as JSON; the composer already requires two (400 options: create)
    TooManyRequests,      // a rate limit (429: reminder; 60 an hour, or the server's brute-force throttle)
};

inline TalkRefusal classifyTalkRefusal(TalkRequest request, int status, const std::string &error)
{
    if (status <= 0)
        return TalkRefusal::NoConnection;

    switch (request) {
    case TalkRequest::EditMessage:
        if (status == 413) return TalkRefusal::TooLong;
        if (status == 400 && error == "age") return TalkRefusal::TooOldToEdit;
        if (status == 400 && error == "message") return TalkRefusal::EmptyText;
        if (status == 403 && error == "permission") return TalkRefusal::NotYourMessage;
        if (status == 404 && error == "message") return TalkRefusal::MessageGone;
        if (status == 405 && error == "message") return TalkRefusal::WrongKind;
        break;
    case TalkRequest::DeleteMessage:
        // A 404 "message" is either a message that is not there or a file message whose share
        // is already gone (then the message stays): not one reason, so it is left generic.
        if (status == 403 && error == "permission") return TalkRefusal::NotYourMessage;
        if (status == 405 && error == "message") return TalkRefusal::WrongKind;   // incl. already deleted
        break;
    case TalkRequest::PinMessage:
        if (status == 404 && error == "message") return TalkRefusal::MessageGone;
        if (status == 400 && error == "message") return TalkRefusal::WrongKind;
        if (status == 403 && error.empty()) return TalkRefusal::NotModerator;
        break;
    case TalkRequest::EditScheduled:
        if (status == 413) return TalkRefusal::TooLong;
        if (status == 400 && error == "send-at") return TalkRefusal::PastTime;
        if (status == 400 && error == "message") return TalkRefusal::EmptyText;
        [[fallthrough]];
    case TalkRequest::CancelScheduled:
        if (status == 404 && error == "message") return TalkRefusal::ScheduledGone;
        if (status == 404 && error == "actor") return TalkRefusal::NotAMember;
        break;
    case TalkRequest::CreatePoll:
        if (status != 400) break;
        if (error == "draft") return TalkRefusal::DraftNeedsModerator;
        if (error == "room") return TalkRefusal::NotAGroup;
        if (error == "question") return TalkRefusal::QuestionTooLong;
        if (error == "options") return TalkRefusal::AnswersTooLong;
        break;
    case TalkRequest::VotePoll:
        if (status == 400 && error == "poll") return TalkRefusal::PollEnded;
        if (status == 404 && error == "poll") return TalkRefusal::PollGone;
        if (status == 400 && error == "options") return TalkRefusal::TooManyChoices;
        break;
    case TalkRequest::ClosePoll:
        if (status == 400 && error == "poll") return TalkRefusal::PollEnded;
        if (status == 404 && error == "poll") return TalkRefusal::PollGone;
        if (status == 403 && error == "poll") return TalkRefusal::PollNotYours;
        break;
    case TalkRequest::SetReminder:
        if (status == 404 && error == "message") return TalkRefusal::MessageGone;
        if (status == 429) return TalkRefusal::TooManyRequests;
        break;
    }
    return TalkRefusal::Unknown;
}

} // namespace talq

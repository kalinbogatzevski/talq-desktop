#pragma once

// Whether the composer may schedule what is in its box -- and whether "send silently" means
// anything for it.
//
// "Send later" posts the box's TEXT to the schedule endpoint, which takes a message and nothing
// else (Talk 24 ChatController::scheduleMessage: message, sendAt, replyTo, silent, thread). It only
// means something when the box holds a plain message about to be written. In every other state it
// does something the user did not ask for:
//   - editing: the buffer is an existing message's text; scheduling it posts a NEW copy later and
//     leaves the editing bar up over an emptied box;
//   - a file staged, or still being encoded: only the caption would be scheduled, the box cleared
//     and the file left staged;
//   - recording a voice note: the box is hidden (not reachable today; kept as defence in depth).
// Empty and too-long are not about the state but about the text; they sit last so that the state,
// which explains the refusal best, wins when several apply. Pure and Qt-free, like
// MessageLengthPolicy.h; the composer words the reasons.

namespace talq {

enum class ScheduleBlock {
    None,
    Editing,
    StagedFile,    // a file is staged or its image is still being prepared
    Recording,
    Empty,
    TooLong,
};

inline ScheduleBlock scheduleBlockFor(bool editing, bool stagedFile, bool recording,
                                      bool textEmpty, bool tooLong)
{
    if (editing)    return ScheduleBlock::Editing;
    if (stagedFile) return ScheduleBlock::StagedFile;
    if (recording)  return ScheduleBlock::Recording;
    if (textEmpty)  return ScheduleBlock::Empty;
    if (tooLong)    return ScheduleBlock::TooLong;
    return ScheduleBlock::None;
}

// "Send silently" sets a flag that only a plain message honours: an edit is sent as the original
// was (Talk keeps the original's silent flag) and a file share carries no silent flag at all. The
// menu must not promise "no notification" where it cannot be kept.
inline bool silentSendIsHonoured(bool editing, bool stagedFile)
{
    return !editing && !stagedFile;
}

} // namespace talq

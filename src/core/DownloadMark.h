#pragma once

// Mark-of-the-Web for a file TalQ saves from a chat.
//
// A file a browser downloads carries a "Zone.Identifier" alternate data stream that names the zone
// it came from, and Windows reads it when the file is opened: a program from the Internet zone is
// checked and the user warned before it runs. A file TalQ wrote from a chat had none, so an .exe
// received in a chat and opened from TalQ ran without that warning. Every received file TalQ
// writes to disk goes through MessageListModel::fetchDavFile (Download, the click that opens a
// file, the image viewer's "Save as...", the voice-message cache), and it is marked there, before
// anything is told the file exists, the way browsers do it:
//
//   1. Windows' Attachment Services (IAttachmentExecute: SetClientGuid with TalQ's fixed GUID,
//      SetLocalPath, SetSource with the address the bytes came from, Save). Windows maps the
//      address to its zone, writes the stream as its policy says, and runs the registered
//      antivirus check, which may delete the file.
//   2. When that interface cannot be used or fails for another reason, TalQ writes the stream
//      itself, always the Internet zone (DownloadMarkLogic.h).
//
// A failure to mark never fails the save: on a FAT/exFAT drive or a share without streams there
// is nowhere to write the mark, and the file is kept with one log line. A file the policy BLOCKS
// or the antivirus FLAGS (Save answers E_FAIL) is removed (if Windows left it), one the check
// REMOVED is reported, and none of them is announced or opened. Another unexpected answer from
// Save keeps the fallback of step 2. E_FAIL is the generic failure code, so a Save that failed
// with it for another reason also costs the file (fail closed): the user is told and can download
// it again.
//
// Not covered: the zone is Windows' mapping of the server's address, so a server the machine
// puts in the Local intranet or Trusted sites zone gets that zone and no warning, as it would in a
// browser; files saved by earlier builds stay unmarked; the update installers are not marked here
// (UpdateChecker has its own checks). Off Windows nothing is done.

#include <QString>
#include <QUrl>

#include <functional>

class QObject;

namespace talq {

enum class DownloadMarkOutcome {
    Marked,           // by Attachment Services
    MarkedDirectly,   // the stream written by TalQ
    NotMarked,        // nowhere to write it (logged); the file is kept and used
    Blocked,          // refused by policy; the file is gone
    VirusFound,       // flagged by the antivirus; removed (if it could be), never used
    Removed,          // removed by the security check; the file is gone
    NotApplicable,    // not Windows
};

// True when the file is there to be announced or opened.
inline bool downloadUsable(DownloadMarkOutcome o)
{
    return o != DownloadMarkOutcome::Blocked && o != DownloadMarkOutcome::VirusFound
           && o != DownloadMarkOutcome::Removed;
}

// Blocking: Save() runs the antivirus check and can take seconds. Never on the GUI thread.
// Logs one line.
DownloadMarkOutcome markDownloadedFile(const QString &path, const QUrl &source);

// Runs markDownloadedFile on a thread of its own and calls `done` on the GUI thread with the
// outcome, unless `context` is gone by then. The file must be closed.
void markDownloadedFileAsync(const QString &path, const QUrl &source, QObject *context,
                             std::function<void(DownloadMarkOutcome)> done);

} // namespace talq

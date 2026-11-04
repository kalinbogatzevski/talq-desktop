#pragma once

// DavSyncPolicy -- pure reading of an HTTP status from a WebDAV request that folder sync made.
// No Qt, no includes: ints in, enum out, the same convention as RestReachabilityPolicy.h. The
// controller maps the finished QNetworkReply onto (httpStatus, aborted) and acts on the outcome;
// the capability probe feeds every probe response through the same function, so the probe and a
// cycle agree on what a status means. See tests/folder_sync_policy_test.cpp.
// Spec: docs/superpowers/specs/2026-11-03-folder-sync-design.md, "ApiClient additions (D16)",
// "Error mapping". The table there is binding; this is that table and nothing more.
//
// The status comes from HttpStatusCodeAttribute (as the repo does everywhere), never from a
// QNetworkReply::NetworkError value. DAV outcomes deliberately do NOT feed
// ApiClient::noteNetworkOutcome, so a failing sync can never flip the app OFFLINE: the notice for
// "offline" comes from the controller's own reading of status 0.
//
//   200, 204 ......................... Ok                  success
//   201 .............................. Created             success
//   404 .............................. NotFound            GET: remote absent, recreate from
//                                                           local; conditional PUT: re-GET
//   412 .............................. PreconditionFailed  NOT an error by itself: re-GET and
//                                                           re-merge (the controller counts the
//                                                           self-inconsistent ones, see
//                                                           SelfInconsistent412Tracker)
//   401, 403 ......................... AuthRefused         pause for the session; never retry,
//                                                           never log out, never "missing"
//   409 .............................. Conflict            unexpected for a root file: pause
//   413 .............................. PayloadTooLarge    its own pause (the server refused the
//                                                           file), not the "too many folders" one
//   423 .............................. Locked              transient (its outcome is its own so
//                                                           a caller can tell it from a timeout)
//   507 .............................. QuotaFull           pause, retry on the slow backoff
//   0, 408, 429, 500, 502, 503, 504 .. Transient           backoff (status 0 = no HTTP answer)
//   aborted .......................... Cancelled           silent
//   anything else .................... Fatal               pause, log the status
//
// Two readings the table leaves open, both fixed by the plan (call 7) and pinned by the test:
//   - a 2xx other than 200, 204 and 201 (202, 206, 207, ...) is Ok. A server that answers a PUT
//     with a success the table does not list has still stored the file, and calling it Fatal would
//     pause sync over a working server.
//   - "anything else" really is everything else: a 1xx, a 3xx, a 600 or a negative number a caller
//     should never pass is Fatal, never Ok and never Transient. Fatal pauses and logs; a wrong
//     "retry" or a wrong "success" could not be taken back.
//
// `aborted` wins over any status: a reply the controller deliberately cancelled (logout, detach,
// disable, account switch) is not a server answer, whatever the reply happened to carry when it
// was cut. It covers THOSE cancels only. A download the controller aborts because the body passed
// the 4 MiB cap (D39) is NOT Cancelled: that is the controller's own RemoteUnreadable, decided
// before classifyDav is consulted, and it must never be routed through `aborted = true` (which
// would make it a silent cancel).

namespace talq {

enum class DavOutcome {
    Ok,                  // 2xx other than 201: the request worked
    Created,             // 201: the file was created
    PreconditionFailed,  // 412: If-Match / If-None-Match did not hold
    NotFound,            // 404
    Conflict,            // 409: a missing parent or a clash; unexpected for the root file
    Locked,              // 423: the file is locked right now
    PayloadTooLarge,     // 413: the server refused the body as too large
    QuotaFull,           // 507: no space left in the user's storage
    AuthRefused,         // 401, 403: the saved sign-in is not accepted
    Transient,           // no HTTP answer, or a status that usually passes: back off and retry
    Cancelled,           // deliberately aborted by us
    Fatal,               // a status the table does not name
};

inline DavOutcome classifyDav(int httpStatus, bool aborted)
{
    if (aborted)
        return DavOutcome::Cancelled;
    switch (httpStatus) {
    case 0:
    case 408:
    case 429:
    case 500:
    case 502:
    case 503:
    case 504:
        return DavOutcome::Transient;
    case 201: return DavOutcome::Created;
    case 404: return DavOutcome::NotFound;
    case 412: return DavOutcome::PreconditionFailed;
    case 401:
    case 403:
        return DavOutcome::AuthRefused;
    case 409: return DavOutcome::Conflict;
    case 413: return DavOutcome::PayloadTooLarge;
    case 423: return DavOutcome::Locked;
    case 507: return DavOutcome::QuotaFull;
    default:
        break;
    }
    if (httpStatus >= 200 && httpStatus < 300)
        return DavOutcome::Ok;
    return DavOutcome::Fatal;
}

} // namespace talq

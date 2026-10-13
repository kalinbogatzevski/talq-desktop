#pragma once
// Aborting every in-flight request, safely.
//
// QNetworkReply::abort() emits finished() SYNCHRONOUSLY (CLAUDE.md pitfall 1), and every handler
// ApiClient attaches to a tracked reply removes that reply from the tracking list as its first
// act -- and then runs the caller's callback, which may start new requests. Looping over the live
// list while that happens mutates the container under the loop's iterators. Qt 6's QList happens
// to survive it today (its iterators are plain pointers into storage that only shrinks), which is
// not something to build a logout path on: CHANGELOG v0.9.4 even claimed the list was copied
// first, but the code never did that.
//
// Take the whole list out first, then abort from the private copy:
//   - every request tracked when the call started is aborted exactly once, however the handlers
//     reshuffle the live list (their removeOne(reply) is a harmless no-op on the emptied list);
//   - a request a callback starts DURING the drain is tracked normally and is left alone -- one
//     pass, never "until empty", so callbacks that re-issue a request on cancel cannot turn the
//     logout into an unbounded drain;
//   - the snapshot holds GUARDS, not raw pointers. A callback may open a modal dialog, and that
//     nested event loop lets other requests in the snapshot finish and be deleted; the drain
//     then skips a guard that went null instead of aborting freed memory.
//
// Header-only and Qt-free: `makeGuard(item)` returns a handle that converts to bool (still alive)
// and has data() (production: QPointer<QNetworkReply>). tests/pending_reply_drain_test.cpp runs
// this exact function.

#include <cstddef>
#include <utility>
#include <vector>

namespace talq {

template <typename Container, typename MakeGuard, typename AbortOne>
void abortAll(Container &pending, MakeGuard &&makeGuard, AbortOne &&abortOne)
{
    using Item  = typename Container::value_type;
    using Guard = decltype(makeGuard(std::declval<Item &>()));

    Container taken = std::exchange(pending, Container{});
    std::vector<Guard> guards;
    guards.reserve(static_cast<std::size_t>(taken.size()));
    for (auto &item : taken)
        guards.push_back(makeGuard(item));
    for (Guard &g : guards)
        if (g)
            abortOne(g.data());
}

} // namespace talq

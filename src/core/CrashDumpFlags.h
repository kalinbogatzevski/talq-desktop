#pragma once

#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>

// kTalqMiniDumpType -- what the crash backstop in main.cpp (talqWriteMiniDump)
// puts in talq_debug.crash.dmp.
//
// Why (2026-09-16): the dump used MiniDumpWithIndirectlyReferencedMemory,
// which copies the heap pages that any stack slot points at. A crashed call's
// dump, once shared, carried the user's Nextcloud app password (14 HTTP Basic
// header copies), session cookies (12) and TURN credentials (8).
// Every one of them sat in those indirectly referenced pages; none was in a
// thread stack.
//
// Kept: every thread's stack and register context (call stacks), the module
// list, thread info (start address, times) and unloaded modules (a fault in a
// plugin that was already unloaded).
// Left out: indirectly referenced memory (the leak) and data segments. Data
// segments are +36 MB on a deployed TalQ (33 MB of it is ICU's data table),
// which is too big to attach to a chat, and they carry the globals of every
// loaded module -- libcrypto's among them -- which nobody can vet for secrets.
// Data segments do not include heap (verified), but they are not needed:
// the GLib state the 2026-09-16 analysis wanted from them belonged to the log
// race that GLibLogWriter.h removes.
// Residual: a secret held in a stack buffer at the moment of the crash is
// still captured.
//
// Pinned by tests/minidump_secret_test.cpp.
inline constexpr MINIDUMP_TYPE kTalqMiniDumpType = static_cast<MINIDUMP_TYPE>(
    MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);

// Dump types that copy heap or other private data pages, i.e. where the
// credentials were. Builds before 2026-09-16 wrote the first one.
inline constexpr ULONG64 kHeapCopyingDumpFlags =
    MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithFullMemory
    | MiniDumpWithPrivateReadWriteMemory | MiniDumpWithPrivateWriteCopyMemory;

// True when `path` is a minidump whose header says it copies heap pages -- a
// talq_debug.crash.dmp left by an older build. Reads only the 32-byte header.
// A kTalqMiniDumpType dump, a missing file or any other file gives false.
inline bool talqDumpCopiesHeap(const wchar_t *path)
{
    const HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    MINIDUMP_HEADER h{};
    DWORD got = 0;
    const BOOL ok = ReadFile(f, &h, sizeof h, &got, nullptr);
    CloseHandle(f);
    // 0x504D444D is MINIDUMP_SIGNATURE ('PMDM', "MDMP" on disk), spelled out
    // because the SDK's multi-character constant warns under -Wall.
    return ok && got == sizeof h && h.Signature == 0x504D444DUL
        && (h.Version & 0xFFFF) == MINIDUMP_VERSION
        && (h.Flags & kHeapCopyingDumpFlags) != 0;
}

#endif // _WIN32

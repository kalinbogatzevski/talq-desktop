# TalQ — Continuation Prompt

Copy-paste this to Claude Code to continue development:

---

I'm continuing development of TalQ, a Qt6/QML Nextcloud Talk desktop client.

**Repo:** `git clone https://gitlab.123net.link/kalin/talk-desktop-qt.git`

Read CLAUDE.md first — it has the full build setup, architecture, pitfalls, and API notes.

**Current state (v0.3.0):**
- Working chat app with SQLite cache, avatars, dark/light mode
- Warm teal theme (#2ec4b6), hybrid message layout (flat others, teal bubble own)
- Emoji reactions, reply-to, optimistic sending, mention resolution
- Right-click context menu with emoji picker + actions
- Page-based layout with TopToBottom ListView (oldest-first storage)
- Inno Setup installer, GitLab CI packaging

**Build setup needed on new machine:**
```bash
pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.2 win64_mingw --outputdir C:\Qt -m qtwebsockets
python -m aqt install-tool windows desktop tools_mingw1310 --outputdir C:\Qt
python -m aqt install-tool windows desktop tools_cmake --outputdir C:\Qt
python -m aqt install-tool windows desktop tools_ninja --outputdir C:\Qt
```
Then create junction if project path has spaces (see CLAUDE.md).

**Priority tasks:**

1. **Threads/Topics** (major feature) — Research done in `docs/research/2026-03-19-talk-threads-research.md`. Display Talk Threads as Telegram-style Topics inside group chats. Talk v22+ has native thread API: `GET /chat/{token}?threadId=X`. Design the UI first (brainstorming skill), then implement.

2. **UX polish:**
   - Server info card missing from welcome page (was in earlier version, lost during ChatView rewrite)
   - Hover action buttons need better positioning (currently can overlap message content)
   - Right-click context menu actions (Forward, Pin, Mark unread, etc.) need implementation
   - Reaction display as clickable pills instead of plain text

3. **Chat features:**
   - Unread message counters per chat (badge in conversation list)
   - Scroll to first unread message when opening a chat
   - File attachments / image preview in messages
   - System tray notifications

4. **Code quality:**
   - MessageCache destructor (close SQLite connection)
   - AvatarProvider thread safety (use QMetaObject::invokeMethod for main thread)
   - Window position restore validation (off-screen check)
   - Code review and simplification pass

Please read CLAUDE.md and the research doc, then let's start with whatever you think is most impactful.

# TalQ — Continuation Prompt

Copy-paste this to Claude Code to continue development:

---

I'm continuing development of TalQ, a Qt6/QML Nextcloud Talk desktop client.

**Repo:** `git clone https://gitlab.123net.link/kalin/talk-desktop-qt.git`

Read CLAUDE.md first — it has the full build setup, architecture, pitfalls, and API notes.

**Current state (v0.5.2):**
- Working chat with push notifications (Notify Push WebSocket), typing indicators (HPB signaling)
- System tray with unread badge, minimize-to-tray, notification sounds
- File upload (📎 button + drag-and-drop), inline image previews (authenticated provider)
- 123NET branded build (`cmake -DTALQ_BRAND=123NET`), separate installer
- Read markers, user online status dots, date separators, font zoom
- Dark/light mode, warm teal theme (#2ec4b6)
- SQLite cache (worker thread), conversation list in-place updates
- Threads/Topics scaffolding (data layer + QML, UI disabled pending polish)
- Self-signed code signing: 123 NET CPT (PTY) LTD
- Inno Setup installers for both generic and 123NET builds

**Build setup needed on new machine:**
```bash
pip install aqtinstall
python -m aqt install-qt windows desktop 6.8.2 win64_mingw --outputdir C:\Qt -m qtwebsockets
python -m aqt install-tool windows desktop tools_mingw1310 --outputdir C:\Qt
python -m aqt install-tool windows desktop tools_cmake --outputdir C:\Qt
python -m aqt install-tool windows desktop tools_ninja --outputdir C:\Qt
```
Then create junction if project path has spaces (see CLAUDE.md).

**Build commands:**
```bash
export PATH="/c/Qt/Tools/mingw1310_64/bin:/c/Qt/Tools/CMake_64/bin:/c/Qt/Tools/Ninja:$PATH"
cmake -B C:/build/talq -S C:/src/talk-desktop-qt -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/Qt/6.8.2/mingw_64 -DCMAKE_CXX_COMPILER=g++ -Wno-dev
cmake --build C:/build/talq
export PATH="/c/Qt/6.8.2/mingw_64/bin:$PATH"
QT_FORCE_STDERR_LOGGING=1 /c/build/talq/talq.exe
```

**Priority tasks:**

1. **Typing indicators debug** — HPB signaling connects and room join works, but typing from TalQ → other clients not confirmed. Need to verify the message format matches what the web client sends. Spreed source cloned at `C:\src\spreed` for reference.

2. **In-app popup notifications** — Telegram-style floating preview in the corner when a new message arrives in another conversation. Currently only Windows toast notifications.

3. **Context menu positioning** — Menu clips inside window. Should use top-level popup or scroll within bounds.

4. **Message cache re-enable** — Cache loads disabled due to UI freeze. Need to load cached messages without blocking (fully async with small batches).

5. **Threads/Topics polish** — ThreadListView disabled in ChatView. Re-enable after testing thread data fetching and navigation.

6. **123NET branding polish** — Background watermark, custom accent color (#38bdf8 sky blue), more professional login screen.

7. **Code quality** — Code review findings partially addressed. Remaining: extract shared QML components (reactions, reply quotes), persistent ID set for O(1) dedup, naming consistency.

Please read CLAUDE.md and let's start with whatever you think is most impactful.

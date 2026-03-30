# TalQ v0.14.5 Continue Prompt

## Current status
Full QWidget app. Released v0.14.5. QPainter rendering, no QML.
Installers now ship all GStreamer transitive DLLs (23 were missing — calls unavailable on clean machines).
**Audio calls still broken via MCU** — Janus says `Unsupported codec 'none'` because the HPB signaling server creates rooms without specifying audiocodec. See "Active bug" below.
Ilko confirmed: call notifications now working (auto-refresh fix), hangup no longer freezes (async teardown).

## Machine setup

### HOME machine (primary dev)
- **Repo:** `C:\Users\bogat\Desktop\My Projects\talk-desktop-qt`
- **Junction:** `C:\src\talk-desktop-qt` → above path
- **Claude Code working dir:** `C:\src` (start claude from here)
- **Claude memories:** `C:\Users\bogat\.claude\projects\C--src\memory\`
- **MSYS2:** `C:\msys64` (GStreamer packages installed)
- **Qt:** `C:\Qt\6.8.2\mingw_64`
- **Build dirs:** `C:\build\talq` (debug), `C:\build\talq-release`, `C:\build\talq-123net`

### OFFICE machine (first-time setup)
Run these once from an **admin** command prompt to create the same directory layout:

```cmd
:: Create C:\src junction (same as home machine)
mkdir C:\src 2>nul
mklink /J C:\src\talk-desktop-qt "C:\Users\bogat\Desktop\My Projects\talk-desktop-qt"

:: Create C:\build for build output
mkdir C:\build 2>nul
mkdir C:\build\talq 2>nul

:: Install MSYS2 if not present (needed for GStreamer + runtime DLLs)
:: Download from https://msys2.org, install to C:\msys64
:: Then in MSYS2 terminal:
:: pacman -S mingw-w64-x86_64-gstreamer mingw-w64-x86_64-gst-plugins-base mingw-w64-x86_64-gst-plugins-good mingw-w64-x86_64-gst-plugins-bad mingw-w64-x86_64-gst-plugins-ugly
```

**IMPORTANT:** Create the junction from an **admin** prompt. Junctions created by non-admin users get "untrusted mount point" status in Qt6 — admin-created junctions are trusted.

Then set up Claude Code:
```bash
# Create Claude memory dir for C:\src working directory
mkdir -p ~/.claude/projects/C--src/memory

# Copy memories from old working dir (if any exist)
cp -r ~/.claude/projects/C--Users-bogat/memory/* ~/.claude/projects/C--src/memory/ 2>/dev/null

# Start Claude from C:\src
cd /c/src
claude
# Say: "read the continue.md"
```

### Unified paths (both machines)
| Path | Purpose |
|------|---------|
| `C:\src\talk-desktop-qt` | Source (junction on both machines) |
| `C:\build\talq` | Debug build |
| `C:\build\talq-release` | Generic release build |
| `C:\build\talq-123net` | 123NET branded release build |
| `C:\msys64` | MSYS2 (GStreamer, runtime DLLs) |
| `C:\Qt\6.8.2\mingw_64` | Qt SDK |
| `C:\Users\bogat\.claude\projects\C--src\memory\` | Claude memories |

## What was done (v0.14.5, 2026-03-30)

### Installer — GStreamer DLLs
- Installer was missing 23 transitive DLLs (libnice, libopus, libsrtp2, OpenSSL, GnuTLS chain etc.)
- build-release.sh and deploy-dev.sh now copy all deps; plugins sourced directly from MSYS2

### Call fixes
- `conversations.startAutoRefresh()` added to startServices (was never called)
- ICE candidate queuing in all 3 pipeline types (Peer/Publish/Subscribe)
- Audio pad link return check (was silently failing)
- Async pipeline teardown (gst_element_set_state off UI thread)
- SSRC sync: payloader SSRC set to match SDP after offer creation

### File caption
- Captions now sent via `talkMetaData.caption` in share API (not separate message)

## What was done (v0.14.0–v0.14.4, 2026-03-29)

### Multi-message selection (Telegram-style)
- Drag-to-select: click and drag sweeps over messages to select them
- Right-click → "Select" or Ctrl+Click to enter selection mode, Esc to exit
- Teal row highlight + circular checkboxes on the right
- Action bar replaces composer: Forward, Copy, Delete, Cancel
- Copy formats as `[Author, HH:MM]\nMessage` per message
- Forward: conversation picker dialog, posts messages as text to target
- Delete: bulk delete with confirmation (only when all selected are own)
- Ctrl+C shortcut copies in selection mode
- Drag-to-scroll removed — scroll via mouse wheel + scrollbar only

### Chat layout redesign
- Unified left-aligned layout — all messages (own + others) left-aligned with avatar column
- Own message avatars shown for non-grouped messages
- Author name above bubble (not inside)
- Bubbles computed in LayoutEngine with proper padding (10px horiz, 6px vert)
- Other-person messages get subtle transparent bubble background
- Timestamp right-aligned inside bubble, with read status icon for own
- Hover reply/react buttons right of bubble with proper gap
- Own messages: reply button only (no react). Others: react + reply

### Notifications
- Custom NotificationPopup — frameless dark rounded popup at bottom-right
- Click notification opens the conversation (restores window + selects chat)
- Shows for cross-chat messages even when app is focused
- Fixed: was showing oldest message instead of newest (model is newest-first, index 0)

### File upload
- Upload progress bar above composer (filename + percentage + teal progress line)
- Caption typed in main composer input (removed separate caption field)
- Enter key sends pending file with caption
- Pending bar: file preview + name + cancel only (send via composer)
- Scroll to bottom on messageSent
- Junction resolution: Qt6 blocks NTFS junction traversal; resolves each path component via isJunction() + junctionTarget()
- Temp-copy fallback if direct open still fails
- Error dialog for upload failures (errorOccurred signal wired)

### Other fixes
- Per-user install path (AppData\Local\Programs, no admin needed)
- Instant read status — push events trigger messages.refresh()
- Chat scrollbar — thin scrollbar thumb painted in ChatPainter
- Reaction counts — fixed showing 0 (was toInt on array, now uses array.size())
- Composer focus proxy for reply-to-focus
- HTML stripping uses QTextDocument::toPlainText() instead of regex
- Online status live updates — header refreshes when user statuses change
- File size displayed in attachment pills (KB/MB)
- Sidebar last message preview updates when new messages arrive
- build-release.sh script for reliable installer builds
- All GitLab releases created with changelog (v0.1.0–v0.14.4)

## Active bug: Video SSRC mismatch — Janus drops TalQ's video RTP

### What works
- **Audio calls**: bidirectional through MCU ✓
- **Receiving browser video in TalQ**: subscriber pipeline ✓
- **Camera toggle**: no freeze (permanent encoder + source swap) ✓
- **Camera preview**: local appsink preview ✓

### What doesn't work
- **TalQ's camera video → browser**: Janus drops all video RTP with "Unknown SSRC"

### Root cause (confirmed with debugging)
GStreamer's `webrtcbin` internal `rtpbin` **rewrites the SSRC on the wire** to a different value than what appears in the SDP offer. The payloader's SSRC matches the SDP, but rtpbin overrides it.

Evidence from debug logging:
- Payloader caps SSRC = SDP SSRC (4190602365) — they match
- But Janus sees SSRC 793065202 — rtpbin's internal session SSRC
- Audio has NO `a=ssrc` lines in SDP and works (Janus learns SSRC from first packet)
- Video HAS `a=ssrc` lines in SDP and fails (Janus validates against them)

### What was tried
1. Sync payloader SSRC to match SDP → rtpbin overwrites it anyway
2. Pre-set payloader SSRC before pipeline starts → webrtcbin ignores it in SDP
3. Strip video `a=ssrc` from SDP → Janus still rejects (may need `mid` extension)
4. Rewrite SDP with rtpbin's internal SSRC → can't access (webrtcbin doesn't expose rtpbin property, session 1 returns NULL)
5. Input-selector approach → worked for test sources, SSRC still wrong on wire

### Browser comparison
- Browser: `RTCPeerConnection` owns the whole pipeline, SSRC is consistent by design
- GStreamer: separate elements (payloader, rtpbin) with independent SSRC management
- Browser offer has `m=video <real-port>`, GStreamer uses `m=video 0` with `a=bundle-only`
- Janus 1.1.4 may not support `a=bundle-only` → fixed by munging to port 9

### Next steps to fix
1. **Enable Janus verbose logging** (`debug_level=7`) to see exactly what Janus expects vs receives
2. **Capture browser's SDP** from Chrome DevTools → compare line-by-line with TalQ's
3. **Try `janusvrwebrtcsink`** from gst-plugins-rs — handles all Janus quirks
4. **Try disabling rtpbin SSRC rewriting** — set `internal-ssrc` on rtpbin session to match SDP
5. **Alternative: P2P for 1:1 calls** — bypasses Janus entirely

### Server access
- SSH: `ssh root@ncloud.123net.link`
- Janus config: `/opt/talk-hpb/janus.plugin.videoroom.jcfg`
- Signaling config: docker `talk-hpb_signaling_1` at `/config/server.conf`
- Janus logs: `docker logs talk-hpb_janus_1 2>&1`
- NC Talk source (cloned): `C:\src\spreed`

## Known bugs
- **Video SSRC mismatch** — see "Active bug" above
- "Unsupported codec 'none'" on subscriber — server config issue (affects all clients)
- Duplicate message flash on send (optimistic send + poller overlap race)
- Notification stacking — single popup replaces previous, doesn't queue
- Notification always appears on primary screen, not the app's screen

## Next steps
- **Fix video SSRC** — priority #1, see "Active bug" section
- **Screen sharing** — d3d11screencapturesrc
- **Background blur** — Windows Studio Effects API or MediaPipe
- In-bubble text selection
- Notification stacking/monitor
- Hardcoded dark theme colors in SelectionBarWidget/ConversationPickerDialog — use PainterTheme
- Cancel upload button on progress bar

## Architecture notes

### Key files
| File | Purpose |
|------|---------|
| `src/painter/ChatPainter.cpp` | QPainter message rendering, selection, hover bar, scrollbar |
| `src/painter/LayoutEngine.cpp` | Message layout computation (unified left-aligned) |
| `src/painter/PainterTheme.h` | Theme constants, colors, fonts, spacing |
| `src/ui/MainWindow.cpp` | Main window, signal wiring, context menu, selection bar |
| `src/ui/ComposerWidget.cpp` | Message input, file paste/attach, pending bar |
| `src/ui/SelectionBarWidget.cpp` | Selection mode action bar (Forward/Copy/Delete/Cancel) |
| `src/ui/ConversationPickerDialog.cpp` | Forward target picker |
| `src/ui/NotificationPopup.cpp` | Custom notification popup |
| `src/models/MessageListModel.cpp` | Messages (newest-first), upload, forward |
| `src/models/ConversationListModel.cpp` | Conversation list, unread, user status |
| `src/main.cpp` | App init, push→refresh wiring, notification wiring |
| `scripts/build-release.sh` | Release installer build script |
| `scripts/deploy-dev.sh` | Debug build deployment |

### Message rendering
- ChatPainter: QPainter-based, all messages left-aligned
- LayoutEngine: computes MessageLayout with bubbleRect, contentX, contentW
- bubblePadX = 10, bubblePadTop/Bottom = 6 — content inside bubble with padding
- bubbleRect computed in layout, painting uses it directly (no ad-hoc computation)
- contentRight = bubbleLeft + bubbleW — hover buttons positioned from this
- Selection state: m_selectionMode + m_selectedIds (QSet<int>)

### Build commands
```bash
# Debug build + run
/c/Qt/Tools/CMake_64/bin/cmake.exe --build /c/build/talq --target talq
cd /c/build/talq && bash /c/src/talk-desktop-qt/scripts/deploy-dev.sh

# Kill running TalQ
cmd.exe //c "taskkill /IM talq.exe /F"

# Release installers (nukes build dir, full clean build)
bash scripts/build-release.sh              # generic
bash scripts/build-release.sh --brand 123NET  # branded

# Upload to GitLab package registry
PAT=$(echo "protocol=https\nhost=gitlab.123net.link\n" | git credential fill | grep password | cut -d= -f2)
curl --header "PRIVATE-TOKEN: $PAT" --upload-file dist/TalQ-v{VER}-Setup.exe \
  "https://gitlab.123net.link/api/v4/projects/13/packages/generic/talq/{VER}/TalQ-v{VER}-Setup.exe"
```

### Installer
- Inno Setup: `C:\Users\bogat\InnoSetup\ISCC.exe`
- Generic: `installer/talq-setup.iss` → installs to `{localappdata}\Programs\TalQ`
- 123NET: `installer/123net-talk-setup.iss` → installs to `{localappdata}\Programs\123NET TalQ`
- Per-user install (PrivilegesRequired=lowest), no admin, no junction traversal issues

### Testers
- **Ilko** (Talk token: `ycy3ht4n`) — gets **generic** TalQ installer
- **Rakesh** (Talk token: `bv86wo4c`) — gets **123NET branded** installer

### Call flow (MCU mode)
1. startCall → POST /call/{token} → join call on server
2. PublishPipeline: always starts with dummy 16x16 VP8, camera replaces if available
3. Offer sent to own session (HPB creates Janus publisher room)
4. Remote joins → requestOffer → SubscribePipeline receives remote audio/video
5. ICE: STUN + TURN servers from /signaling/settings
6. Media state broadcast via signaling mute/unmute messages
7. Hangup: DELETE /call/{token}?all=true + teardown pipelines

### Video display
- VideoFrameProvider converts I420 GstSample → QImage via BT.601 YUV→RGB
- CallDialog::VideoWidget paints QImage with QPainter (aspect-ratio preserving)
- Remote video shown only when frames >32px and remote video not muted
- Local preview: small 120x90 overlay positioned at bottom-right of remote video

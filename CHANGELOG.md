# Changelog

## v0.23.1 (2026-04-21)

### Fixes — image paste
- **No more UI freeze on paste** — PNG encoding moved off the main thread (`QtConcurrent::run` + `QFutureWatcher`). Large clipboard images no longer lock up the window.
- **Visible "Preparing image…" feedback** — the pending-file bar now shows an hourglass immediately when you paste, replaced with the real thumbnail when encoding finishes.
- **Thumbnails appear for freshly sent images** — Nextcloud's preview service is async and 404s for a few seconds after upload. Thumbs used to get cached as empty on that first miss and never retry. Now retries with `1.5s → 3s → 6s → 12s` backoff (4 attempts, capped at 30s).

### Fixes — PR-review backlog
- **Teal accent now consistent everywhere** — swept `#2ec4b6` → `#14b8a6` in Message/CallDialog/SharePicker/SelectionBar/ConversationPicker that earlier missed the warm-dispatch refactor.
- **Memory leak fix — `setChatThreadTitle`** — the `shared_ptr<function>` self-reference cycle from v0.23.0 is gone; the endpoint-chain is now a heap state machine that explicitly self-deletes on terminal.
- **Memory leak fix — group creation** — the shared counter/errors used while inviting participants now use `shared_ptr`, so they're freed if the dialog is dismissed mid-flight.
- **Wrong-conversation race fix — new topic** — creating a topic while switching rooms no longer yanks you into the topic view on the wrong conversation.
- **Member list UX — right-click for actions** — left-clicking a member row no longer pops a context menu on every click. The promote/demote/remove menu is on right-click only, and doesn't appear at all on non-actionable rows (yourself, owner, or if you're not a moderator).

## v0.23.0 (2026-04-21)

### Conversation management
- **Group info dialog** — new ℹ button in the chat header opens a modal to rename the group, edit description, add/remove members, promote/demote moderators, leave, or (owner only) delete. Reachable from any group or public room.
- **Promote & demote** — click a member in the info dialog → context menu with promote-to-moderator, demote-to-member, or remove actions.

### Topics, properly
- **Telegram-style topic strip** replaces the old 3-column threads panel. Horizontal chip bar above the chat: "All messages", every topic as its own chip with unread count, trailing "+" for new.
- **One-click topic creation** — click +, type a name, done. TalQ sends a silent seed message and best-effort sets the thread title; you land in the new topic ready to chat.
- **Threads clear on group switch** — the old list no longer leaks into the new group during the fetch round-trip; cache path now actually runs on switch.
- **1-on-1 chats correctly hide the topic strip** — previously flickered visible during model reset.

### Design — "warm dispatch" identity
- **Inter** bundled as the body font across the entire app; consistent typography from sidebar to composer.
- Warm paper-dark palette replaces flat gray.
- **Electric teal** accent (`#14b8a6`) with proper hover/pressed states.
- **Segoe Fluent Icons** for all chrome (sidebar controls, header buttons, composer). Bell icon for reminders now renders consistently across Windows versions.
- Dialogs (New Chat, Conversation Info, Upcoming Reminders, Nextcloud File Picker) share a token system — same eyebrow labels, same primary-button shape, same rounded corners.
- Composer input corners round properly now (QTextEdit viewport transparency fix).
- Sidebar preview (last message under chat name) uses readable secondary text color.
- `EmojiTextRenderer::elide` bug fix — long one-word previews used to collapse to just "…"; now they trim character-by-character.

### ApiClient additions
- `setRoomName`, `setRoomDescription`, `deleteRoom`, `leaveRoom`
- `fetchRoomParticipants`, `removeRoomParticipant`, `promoteModerator`, `demoteModerator`
- `sendChatMessage` (returns new message id), `setChatThreadTitle` (best-effort chained endpoint attempts)

### Fixes
- Description field can no longer silently overwrite the server value with an empty string on focus change (guard via `QLineEdit::isModified`).

## v0.22.1 (2026-04-20)

### Fixes
- **Chat header clears when returning to Home** — stale conversation name stayed visible; `m_header` now hides on Home navigation and shows on conversation open.
- **Update banner stays visible across navigation** — switching between Home and a chat used to leave the banner in an unpredictable state. Added an internal active flag so once the banner is shown, it's re-shown and Z-raised whenever the chat-column contents switch.

## v0.22.0 (2026-04-20)

### New chats
- **Create new conversations from TalQ** — ➕ button in the sidebar (next to ⚙ settings) opens a **New chat** dialog.
  - **Direct:** search for a user, double-click → creates a 1-on-1 conversation and opens it.
  - **Group:** name the group, search and add multiple participants, create → room is created and everyone invited in one step.
  - User autocomplete via `GET /ocs/v2.php/core/autocomplete/get?itemType=call`.
  - Room creation via `POST /ocs/v2.php/apps/spreed/api/v4/room` (`roomType=1` for direct, `2` for group).
  - Group participants added via `POST /room/{token}/participants`.

## v0.21.0 (2026-04-20)

### Reminders
- **Right-click any message → "Remind me…"** with quick presets (20 min, 1 h, 3 h, Tomorrow 8:00, Next Monday 9:00) and a custom date/time picker. Server persists the reminder and sends a push notification when the time comes.
- **⏰ Upcoming reminders** button in the chat header opens a list of all pending reminders across conversations. Double-click to jump to the original message.
- Internal `Reminder` struct carries a `source` enum (`NextcloudTalk`, `Erp`) so future ERP integration can contribute entries into the same upcoming-reminders view.

## v0.20.2 (2026-04-19)

### Hardening from PR review

- **Share errors now say what actually went wrong** — the picker and the share dialog both branch on HTTP status (401 "session expired", 403 "not permitted", 404 "no longer exists", 503 "maintenance", …) and include the server's OCS `meta.message` when present. Previously everything collapsed to "Are you offline?" / "HTTP 400".
- **Context-safe share callback** — `ApiClient::shareNextcloudFileToChat` now takes a `QObject *context` and disconnects automatically if the caller dies, matching `listNextcloudFolder`'s existing contract. Prevents a theoretical dangling-`this` crash if the composer were destroyed mid-share.
- **PROPFIND treats non-207 responses as failure** — an HTML captive-portal page (common on corporate networks that intercept) used to parse as "empty folder"; now surfaces as an explicit error.
- **PROPFIND checks `QXmlStreamReader::hasError()`** — malformed/truncated XML was silently producing a short list reported as success; now fails loudly.
- **`--debug` log-setup failure is visible** — if `freopen(stderr)` fails (AppData on unreachable share, permissions, AV holding the file), TalQ now shows a `MessageBox` explaining why there's no log, instead of silently running with logging defeated.
- **fileId widened to `qint64`** — NC fileids routinely exceed 2³¹ on busy instances.

## v0.20.1 (2026-04-19)

### Fix
- **Share from Nextcloud now actually works** — v0.20.0 used the wrong endpoint (`/chat/{token}/share`, which is for generic objects like polls). Switched to the Files Sharing API (`/apps/files_sharing/api/v1/shares`) with `shareType=10` (Talk conversation) + `shareWith=<roomToken>` — same call the official NC Talk web frontend makes. Server rejected the old call with HTTP 400.

## v0.20.0 (2026-04-19)

### Share from Nextcloud
- **Attach → From Nextcloud** — the 📎 button now opens a menu with "From this device…" and "From Nextcloud…". The Nextcloud path opens a file browser showing your Nextcloud Files; pick a file and TalQ tells the server to create a share link and post it into the chat (matching how the official NC Talk client works). WebDAV `PROPFIND` for listings, POST to `/ocs/v2.php/apps/spreed/api/v1/chat/{token}/share` to post the link.
- **Breadcrumb navigation** — clickable path segments at the top; double-click a folder to dive in, double-click a file to share immediately.

## v0.19.2 (2026-04-19)

### Fixes
- **Auto-upgrade now actually relaunches** — the `[Run]` section of the installer had the `skipifsilent` flag, which made Inno Setup skip the post-install launch during `/VERYSILENT` installs. After an auto-upgrade, the old TalQ quit but the new one never came back up. Removed the flag so silent installs auto-launch the freshly installed binary.

## v0.19.1 (2026-04-19)

### Composer polish
- **Multi-line input that actually grows** — the message box now expands as you add lines (up to ~5) instead of staying locked at one line. Existing line still hidden behind paragraph returns finally visible.
- **"New messages" divider clears on engagement** — clicking in the composer, typing, or sending a message now dismisses the divider immediately. Previously only scrolling past it dismissed it, which felt stale when you'd clearly acknowledged the new messages.

### Diagnostic
- **`--debug` CLI flag** — Release builds now accept `--debug` (or `-d`) to write a verbose log to `%APPDATA%/talq_debug.log` including GStreamer/Qt warnings. Helpful for remotely diagnosing startup problems on tester machines. Also redirects stderr into the same file so anything printed at C-level is captured.

## v0.19.0 (2026-04-19)

### Auto-upgrade
- **Auto-update from ncloud** — TalQ now checks a manifest on `ncloud.123net.link` at startup and every 4 hours. When a newer version is available, a teal banner appears at the top of the chat: "Install now", "Later", or "What's new" (release notes popover). Downloads stream to the temp folder, verify against SHA-256 from the manifest, then launch Inno Setup silently (`/VERYSILENT /CLOSEAPPLICATIONS /RESTARTAPPLICATIONS`) so the session restarts into the new version automatically.
- **Call-safe** — if you're in a call or sharing your screen when the update is ready, the launch is deferred until the call ends; the banner says so.
- **Opt-out** — Settings → Updates → "Automatically check for updates" toggle. On by default.
- **Release script integration** — `scripts/build-release.sh` uploads both installers + a freshly generated manifest when `NC_APP_PASSWORD` is set, so every future release reaches clients automatically.

## v0.18.0 (2026-04-18)

### Presence
- **Rich status line** — chat header now shows the peer's custom status message + icon (e.g. "🎯 in a meeting") under the conversation title. Falls back to "Online" / "Away" / "Do not disturb" / "Offline" when no custom message is set. Typing state still takes precedence.
- **Multi-color sidebar dots** — away shows amber, DND red, offline hidden; previously only online was surfaced.
- **Dedicated 60-second poll** — status now refreshes on its own timer instead of riding on the conversation-list poll, so changes appear faster.

### Message search
- **In-conversation search** — 🔍 button in the chat header opens a search bar. Results dropdown shows actor + snippet. Click a result to scroll to the message with a 2-second teal flash. If the message is older than what's loaded, history pages backward up to 5 pages automatically.

### Silent send
- **Right-click Send** or **Alt+Enter / Alt+Click Send** to send without triggering notifications. Uses NC Talk server's `silent=true` flag. Send button flashes 🔕 briefly to confirm.

### Time on hover
- Hovering a message's time now shows the full date and time in a tooltip, including the edit timestamp for edited messages.

## v0.17.4 (2026-04-18)

### Fixes
- **Edited messages update in place** — v0.17.2's `editMessage` was double-unwrapping the OCS envelope, which silently replaced your bubble with a blank row. Now the edit shows immediately without a conversation switch.
- **Unread divider auto-dismisses** — once the "New messages" pill has scrolled above the viewport for 2 seconds, it fades out automatically (Discord-style). Switching conversations still resets the state.
- **Scrollbar drag** — grabbing the chat scrollbar now scrolls instead of triggering multi-message selection. Clicking the track jumps the thumb to that point; dragging the thumb follows the cursor.

## v0.17.3 (2026-04-18)

### Unread divider
- **"New messages" pill** — reopening a conversation with unread messages draws a teal accent pill across the chat above the first unread, with a subtle tinted region below it. Matches Nextcloud Talk web.
- Divider is anchored by message id, so scrolling up to load older history keeps it in place.
- Divider is session-frozen: it stays put until you leave the conversation; on the next visit it's at the new boundary or absent.

## v0.17.2 (2026-04-18)

### Message edit
- **Edit your own messages** — right-click an own text message → "✏️ Edit" to open an amber editing bar above the composer pre-filled with the original text. Change the text and hit Enter to update; Esc or the ✕ button cancels. Switching conversations mid-edit cancels safely.
- **"(edited)" marker** — edited messages show `(edited)` prefixed to the time label in the chat, matching Nextcloud Talk web.
- Server enforces the edit window (~6 hours for regular users) and permission rules; failures surface as a toast with the server's error message.
- Edit is not offered on messages with file attachments — server doesn't permit it.

## v0.17.1 (2026-04-18)

### Critical fixes
- **Emoji recents persist across restarts** — `EmojiData::initialize()` now runs after the app/organization names are set, so `QSettings` reads from the correct storage.
- **Mention popup no longer leaks across conversations** — the callback now checks the active token at response time, and switching conversations hides both autocomplete popups and clears pending state.
- **Autoreplace cursor position** — after `:) ` → 🙂, the cursor now sits past the trailing space. Previously the next typed char landed between the emoji and the space.
- **Nextcloud subpath installations supported for mentions** — `fetchMentions` now preserves the URL prefix (e.g. `/nextcloud/...`) instead of stripping it.
- **Paste-image save failure reported** — if Qt can't write the temp file on paste, the user sees a dialog instead of an error when they try to send.

### Important fixes
- **Async callbacks can't use-after-free** — `fetchFileImage` and `fetchMentions` take a `QObject *context` parameter; the connection is severed automatically if the caller widget dies mid-flight. Failures also log HTTP status for debugging.
- **OCS envelope status checked** — `fetchMentions` honors the `ocs.meta.statuscode` so server-side errors (403/404/429 wrapped in HTTP 200) no longer appear as an empty candidate list.
- **Profile avatar failures logged** — the top-right avatar's fetch errors are now visible in the debug log (canary for auth/session problems).
- **`MentionCandidate::Source` is now an enum** — scoped enum with `parseSource()`; prevents future silent classification bugs.

### Polish
- `EmojiRun::widthPx` dead field removed; struct docblock refreshed.
- `ImageViewerDialog` tracks the filename in a dedicated member instead of round-tripping through the title bar's text (fixes rapid-double Ctrl+C visual glitch).
- Phantom 4-pixel home-click strip at the top of the sidebar removed; the 🏠 button is the real entry point.
- Welcome screen shows "Release notes unavailable" instead of an empty panel when the `CHANGELOG.md` resource is missing.
- `fetchFileImage` takes a `maxDim` parameter — the image viewer now sizes requests to the actual screen instead of always asking for 4096×4096.
- `EmojiPickerWidget` emits a dedicated `cancelled()` signal on Esc instead of an empty-string sentinel via `emojiSelected`.

### Refactors (no behavior change)
- `isEmojiCluster` consolidated into `EmojiData` (previously duplicated across `LayoutEngine` and `EmojiTextRenderer`).
- Basic auth extracted to `ApiClient::applyBasicAuth` helper; five callers dedup'd.
- Shortcode walk-back and autocomplete popup stylesheet deduplicated in the composer.
- `ChatPainter::paintMessageEmojis` tightened (dead variable removed, font metrics hoisted out of the loop, line-search extracted as a helper).

## v0.17.0 (2026-04-18)

### Mentions
- **Compose @mentions** — typing `@` in the chat composer opens an autocomplete popup of conversation participants (with avatars) plus `@all`. Picking a row inserts `@userid` (quoted for ids with spaces) into the message; the server then notifies the mentioned user and renders the pill on all clients. Typing `foo@bar.com` does not trigger — email fragments are ignored.

## v0.16.6 (2026-04-18)

### Polish
- **Twemoji in sidebar + thread previews** — the conversation list's last-message preview now renders Twemoji instead of the system emoji font, matching the chat view.
- **Image viewer Ctrl+C / Ctrl+S** — copy the displayed image to the clipboard or save it to disk. A new ⋯ menu button in the viewer's title strip exposes both for mouse users.

## v0.16.5 (2026-04-17)

### Emoji
- **Twemoji everywhere** — chat messages, reactions, composer all render via bundled Twemoji (Nextcloud Talk style). Consistent across Windows versions.
- **Autoreplace** — `:)`, `:D`, `<3`, `:P`, `;)`, etc. → emoji on space. Also `:shortcode:` (Slack-style): `:smile:`, `:rocket:`, `:heart:`, …
- **Emoji picker** — new 😀 button in the composer opens a Telegram-style picker: 9 category tabs, search, recents (24), skin-tone variants via long-press/right-click.
- **Shortcode autocomplete** — typing `:smi` shows a popup of matches; Up/Down/Enter/Tab to pick, Esc to dismiss.

### Image viewer
- New standalone image viewer: **Esc closes**, single-instance (no more stacked viewers on repeated clicks), click-drag to **pan**, Ctrl+wheel to **zoom**, `+`/`-` keys, `0` fits, `1` is 100%, double-click toggles. Opens as its own top-level window with its own taskbar entry. Remembers last geometry.

### Welcome screen
- **What's New panel** — scrollable changelog (this file) rendered as Markdown beside the existing server status card. Collapses to single column below 1100 px.
- **Home navigation** — 🏠 button in the sidebar (next to search) returns to the welcome screen from any open chat.

## v0.16.4 (2026-04-17)

### Bugfixes
- **Multi-message selection** — dragging across messages now enters selection mode without needing to hold Ctrl (regression from v0.15.x char-level text selection work). Drag on body text still does character-level text selection; drag elsewhere (avatar/timestamp/padding) selects whole messages.
- **Infinite scroll history** — scrolling to the top of loaded messages now triggers loading of older history from the server. Viewport position is preserved when older messages are prepended.
- **Installer GPU plugins** — release installer now ships the GStreamer support libraries needed by the `d3d11` and `nvcodec` plugins: `libgstcodecs`, `libgstcodecparsers`, `libgstcuda`, `libgstd3d12`, `libgstdxva`, `libgstgl`. Fixes red "d3d11" and "nvcodec" pills on fresh installs without MSYS2.
- BUILD.md updated to document the full DLL dependency set for the GPU plugins.

## v0.16.3 (2026-04-04)

### Screen sharing — bidirectional
- TalQ→browser screen sharing now works (defer sendoffer until ICE connected)
- Screen/window picker dialog with Screens and Windows tabs
- Monitor selection across multiple displays
- Window capture via d3d11screencapturesrc window-handle
- Proper unshareScreen cleanup (send to self for HPB publisher teardown)
- Camera/share/blur buttons hidden until call reaches Active state

### Bugfixes
- Fixed sendoffer timing — publisher must exist in Janus before notifying peers
- Fixed re-share: send unshareScreen to own session to trigger HPB cleanup
- Single sendoffer on ICE connected (no over-offering/flickering)
- Video fullscreen exits cleanly on call end

## v0.16.0 (2026-04-04)

### Screen sharing
- Receive screen shares from browser users (full MCU signaling support)
- Share button in call dialog (monitor icon)
- Dialog maximizes when viewing screen share, restores when stopped
- Remote camera hidden during screen share (no fighting frames)
- Handles unshareScreen message — clean teardown when remote stops sharing
- dx9screencapsrc capture source (d3d11 fallback for discrete GPU)
- ScreenSharePipeline with roomType "screen" signaling
- SignalingClient extended: roomType on all methods, sendMinimalMessage, sendBroadcastMessage

### Hardware acceleration
- NVIDIA NVDEC VP8 hardware decoding (GPU) with Intel DXVA fallback
- BGRx direct frame path — no CPU YUV→RGB conversion
- Pre-scale large frames in VideoWidget to reduce QPainter load
- GStreamer plugin status pills on welcome page (green=loaded, red=missing)
- GPU acceleration status on welcome page
- Codec + decoder pills in call dialog (VP8/NVDEC/DXVA/Software)

### Video fullscreen
- Double-click any video widget to go fullscreen
- Esc to return to normal view

### Background blur
- Button opens Windows Camera Settings for Studio Effects (NPU hardware)

## v0.15.8 (2026-04-04)

### Data channel media state
- Send audioOn/Off, videoOn/Off on publisher "status" data channel (matches browser Talk protocol)
- Receive media state from subscriber data channels (browser mute/unmute reflected in TalQ)
- Speaking detection with 500ms grace timer — sends speaking/stoppedSpeaking to peers

### In-bubble text selection (Telegram-style)
- Click and drag on message text selects characters with teal highlight
- Drag across messages extends selection seamlessly
- Double-click selects a word; continue dragging extends word-by-word
- Ctrl+C copies selected text; right-click shows Copy menu
- Escape clears selection; links and reactions still clickable

### Screen sharing
- New ScreenSharePipeline: d3d11screencapturesrc (primary monitor, 1080p cap, 30fps, VP8 2Mbps)
- Separate webrtcbin with roomType "screen" (matches browser protocol)
- Share button in call dialog (monitor icon, teal when active)
- Receive screen shares from browser users
- SignalingClient extended with roomType parameter on all methods

## v0.15.6 (2026-04-03)

### Camera architecture — funnel + valve (replaceTrack equivalent)
Based on studying Nextcloud Talk browser source (spreed repo BlackVideoEnforcer):
- **Funnel element** merges dummy and camera sources into shared encoder chain
- Both sources permanently linked — **zero unlinking, zero relinking, zero SRTP errors**
- Camera toggle = two valve property flips (instant, no element recreation)
- **videoconvert + videoscale** between funnel and encoder handles resolution changes
- Camera auto-negotiates resolution (no fixed 720p cap)
- Camera auto-enabled when call reaches Active state (no UI blocking at startup)
- Camera source paused on disable (saves CPU), resumed on enable
- Preview signal disconnected on camera off (prevents frozen frame)
- Remote video reconnects on remote unmute

### Stability
- SRTP seqnum continuity for dummy→camera transition
- `gst_element_link_filtered` invisible capsfilter bug fixed
- Pipeline recovery to PLAYING after transient errors
- cameraChanged signal emitted after disable completes

## v0.15.4 (2026-04-02)

### Outbound video FIXED — camera video now reaches remote
- **Root cause:** v0.15.3 stopped the dummy video source immediately after pipeline start, leaving webrtcbin's video transport dead — its internal rtpbin never saw a video frame, so camera frames linked later were never forwarded to the DTLS/SRTP transport.
- **Fix:** Dummy now runs continuously (16x16 black @ 1fps, negligible bandwidth) keeping the video transport warm. When camera enables, dummy is stopped, `gst_bin_remove`d from the pipeline, and camera links to the same pad with the same SSRC. Seamless swap, no renegotiation needed.
- Confirmed working: TalQ → browser (Talk web app) video call with bidirectional video.

## v0.15.3 (2026-04-02)

### Camera toggle — fully working
- **Direct pad swap** — camera on/off swaps SSRC filters on the same webrtcbin pad. No input-selector, no renegotiation, no element recreation. Instant switch.
- **mfvideosrc fixed** — Media Foundation camera source now loads (added libgstd3d11/libgstd3dshader DLLs). Shared-mode access = no device contention on toggle.
- **Camera capped at 720p** — prevents memory explosion from raw 1080p frames
- **Leaky queues** — max 3 buffers, drops old frames when encoder can't keep up
- **Valve blocks encoding** when camera is off (saves CPU)
- **Camera source paused** on disable (releases device gracefully)
- **Preview hides** on camera off
- **Dialog stays large** when remote stops video but local camera is still on

### Stability
- **2nd-call crash fixed** — QPointer for video provider pointers in CallDialog (use-after-free)
- **Synchronous pipeline cleanup** — all three pipelines (Publish, Subscribe, Peer) use synchronous GST_STATE_NULL instead of detached threads
- **PLI timer thread fix** — QTimer created on Qt thread (was GStreamer thread → crash)
- **SRTP error filtering** — transient transport errors during renegotiation no longer kill camera
- **Pipeline recovery** — audio survives camera errors

## v0.15.1 (2026-04-01)

### Audio fix — v0.15.0 review regression
- **Synchronous pad linking** — v0.15.0 marshalled `onPadAdded` to the Qt thread, creating a race where RTP data arrived before the audio chain was linked. GStreamer returned `NOT_LINKED`, killing all audio. Reverted to synchronous linking on the GStreamer thread.

### Camera toggle fix
- **Dummy video removal** — `enableCamera()` now removes the dummy 16x16 black video track before adding the real camera, preventing a second conflicting video transceiver
- **Transceiver reuse** — camera on/off reuses the same webrtcbin pad instead of creating new ones, avoiding accumulating `m=video 0` lines in the SDP

### Subscriber improvements
- **Re-offer reuse** — MCU re-offers reuse the existing subscriber pipeline (preserves ICE/DTLS connection) instead of tearing down and rebuilding
- **SID tracking** — subscriber signal connections use hash lookup for MCU session IDs, enabling seamless re-offer support
- Removed dead `forceReconnectPublisher()` and unnecessary subscriber re-request on publisher renegotiation

## v0.15.0 (2026-03-31)

### Code Review — 59 issues fixed across 6 review rounds

#### Security
- **HTML injection** — server message content now HTML-escaped before rendering (prevents pixel tracking/IP disclosure)
- **TURN credentials** masked in debug logs
- **Signaling ticket** redacted from log output
- **WebDAV path** sanitization hardened (`?`, `#`, `%`, `..` stripped)
- **HTTPS warning** shown when user enters HTTP server URL
- **File size guard** — uploads over 100MB rejected before loading into memory

#### Crash/Stability
- **QPointer guards** on all 13 GStreamer→Qt callback marshalling sites (use-after-free race)
- **QTimer thread affinity** — onPadAdded marshalled to Qt thread (SubscribePipeline, PeerPipeline)
- **Null pipeline guard** in disableCamera
- **SDP null guard** — set-local-description protected against concurrent cleanup
- **Video provider disconnect** — old provider disconnected before connecting new one
- **invokeMethod target** — onPreviewSample uses QPointer-guarded target
- **Integer overflow** guard in YUV buffer size calculation

#### Protocol Compliance
- **DELETE /call** sends `{all: true}` as JSON body (was query param — hangup was broken for remote)
- **Subscriber re-offer** — stale subscriber evicted and recreated on new offer
- **ICE server race** — offers queued until STUN/TURN servers are fetched

#### Performance
- **Async cache init** — SQLite init no longer blocks UI at startup
- **Async lastCommonRead** — conversation switch no longer blocks on SQLite query
- **Cache saves only new messages** (was saving all 200 on every poll)
- **DebugMonitor** uses QStringList instead of O(n) string trimming

#### Robustness
- **CallSignaling poll delay** — 100ms minimum between polls (was busy-loop)
- **ICE failure recovery** — hangUp on ICE failed in MCU mode
- **acceptCall state guard** — roomJoined callback checks state before proceeding
- **PushClient stop race** — m_stopped flag prevents reconnect after logout
- **Conversation generation counter** — stale async callbacks bail out
- **Participant map pruning** — SignalingClient removes entries when participants leave
- **Camera SSRC capsfilter** cleaned up on disableCamera
- **Subscriber cleanup** — m_pliTimer, m_videoAppsink, m_videoDepay properly nulled
- **Duplicate chain guard** — onPadAdded won't create duplicate audio/video chains
- **trimOldMessages** — now trims oldest (was incorrectly trimming newest)

#### UX Polish
- **Branded splash screen** — dark theme, logo, app name, version, 1.5s minimum display
- **Call dialog centered** on primary screen
- **Mic icon** — text labels "Mic"/"Muted" (was emoji that looked like a light)
- **Squeezed sidebar** — search and settings hidden, avatar only in narrow mode
- **Avatar click** opens settings in all modes
- **Mic level indicator** — teal bar showing live audio signal during calls

#### Other
- **Dead QML provider code** removed (273 lines)
- **Remote video** hides and dialog shrinks when remote stops camera
- **Reaction counts** fixed (array length, not toInt)

## v0.14.7 (2026-03-31)

### Audio calls FIXED — bidirectional through MCU
Root cause: three protocol compliance issues found by comparing TalQ's SDP with browser WebRTC-internals dump:
- **Opus codec format** — GStreamer generated `OPUS/48000` (missing channel count), Janus requires `opus/48000/2`
- **Codec declaration** — signaling offer must include `audiocodec: "opus"` in message data for Janus room creation
- **SSRC consistency** — GStreamer's webrtcbin internal rtpbin rewrites SSRC on the wire; fixed by forcing matching SSRC on both payloader and capsfilter before webrtcbin
- **Data channel** — Janus publisher requires an `m=application` section (data channel) in the SDP; added `status` channel matching browser behavior
- **SSRC lines kept** — `a=ssrc` lines must be present in SDP (not stripped); Janus validates incoming RTP against them

### Video SSRC fix
- Same capsfilter SSRC approach applied to dummy video and camera video pipelines
- Camera SSRC forced via capsfilter between payloader and webrtcbin

### Other improvements
- File caption sent inline via `talkMetaData.caption` (not as separate message)
- Auto-refresh polling for incoming call detection
- ICE candidate queuing (all 3 pipeline types)
- Async pipeline teardown (no more UI freeze on hangup)

## v0.14.5 (2026-03-30)

### Installer fix — calls were unavailable on clean machines
- **GStreamer transitive DLLs** — installer was missing 23 DLLs that GStreamer plugins depend on (libnice, libopus, libsrtp2, OpenSSL, GnuTLS chain). Worked at home because MSYS2 was in PATH; failed on clean installs.
- **build-release.sh** — now copies all transitive deps and pulls GStreamer plugins directly from MSYS2 (no longer depends on a prior debug build)
- **deploy-dev.sh** — same DLL additions for dev build consistency
- **$USER fallback** — uses $USERNAME when $USER is unset (Windows bash)

### Call fixes (in progress)
- **Auto-refresh for call detection** — `startAutoRefresh()` was never called; incoming calls relied 100% on push notifications with no polling fallback
- **ICE candidate queuing** — candidates arriving before remote SDP is set are now queued and flushed after setRemoteOffer/setRemoteAnswer (all 3 pipeline types)
- **Audio pad link diagnostics** — audio receive chain now checks gst_pad_link return value (was silently failing)
- **Hangup freeze fix** — moved gst_element_set_state(GST_STATE_NULL) to background thread (was blocking UI for 20+ seconds during WebRTC teardown)
- **SSRC sync** — publisher payloader SSRC now synced to SDP offer value (Janus drops packets with mismatched SSRC)

### File sharing
- **Caption with file** — captions now sent via talkMetaData.caption in the share API (was sent as a separate message)

## v0.14.4 (2026-03-29)

### Fixes
- **File upload through junctions** — Qt6 blocks NTFS junction traversal; now resolves junctions to real paths before opening files
- **Per-user install** — installs to AppData\Local\Programs (no admin required)
- **Temp-copy fallback** — if direct file open fails, copies to temp before uploading
- **Error dialog** — file upload errors now shown to user instead of silently failing

## v0.14.3 (2026-03-29)

### Fixes
- **Online status live updates** — header status refreshes when user statuses change (was stale after select)
- **File size in attachments** — file pills show size (KB/MB) below filename
- **Sidebar last message** — preview updates instantly when new messages arrive
- **Notification click restore** — uses SetForegroundWindow on Windows to force-bring to front

## v0.14.2 (2026-03-29)

### Notifications
- **Custom notification popup** — Telegram-style dark rounded popup at bottom-right of screen
- **Click notification opens conversation** — restores window and switches to the chat
- **Cross-chat notifications** — shows even when app is focused (different conversation)
- **Notification text fix** — was showing oldest message instead of newest (model index bug)

### File Upload
- **Upload progress bar** — filename + percentage + teal progress line above composer
- **Caption via composer** — type caption in main input, no separate field
- **Enter sends file** — pending file sent on Enter key (with caption from composer)
- **Simplified pending bar** — file preview + name + cancel only, send via composer
- **Scroll to bottom** — chat scrolls down after file upload completes

### Other Fixes
- **123NET branding** — brand logo + TalQ sub-logo on login and welcome screens
- **Instant read status** — push events trigger message refresh
- **Chat scrollbar** — thin scrollbar thumb on right edge
- **Reaction counts** — fixed showing 0 (array length, not toInt)
- **Placeholder consistency** — unified to "Message..." across all code paths
- **Non-branded login** — empty server URL (was hardcoded to 123NET)
- **Dead signal removed** — unused `popupRequested` from NotificationManager

## v0.14.0 (2026-03-29)

### Multi-Message Selection (Telegram-style)
- **Drag-to-select** — click and drag to sweep-select messages
- **Selection mode** — right-click → "Select" or Ctrl+Click to enter, Esc to exit
- **Visual feedback** — teal row highlight + circular checkboxes on the right
- **Action bar** — replaces composer with Forward, Copy, Delete, Cancel buttons
- **Copy** — formats as `[Author, HH:MM]\nMessage` for each selected message
- **Forward** — conversation picker dialog, sends messages as text to target conversation
- **Delete** — bulk delete with confirmation (only available when all selected are own)
- **Ctrl+C** shortcut copies selected messages when in selection mode

### Chat Layout Redesign
- **Unified left-aligned layout** — all messages (own + others) left-aligned with avatar column
- **Own message avatars** — shown for non-grouped messages, same as other users
- **Author name above bubble** — not inside the bubble, with proper full-width display
- **Bubble backgrounds** — own (teal), others (subtle transparent gray), with proper internal padding
- **Timestamp inside bubble** — right-aligned, with read status icon for own messages
- **Proper spacing** — consistent gaps between messages, no bubble overlap

### Fixes
- **Instant read status** — push events trigger message refresh for near-instant read indicators
- **Chat scrollbar** — thin scrollbar thumb on right edge
- **Reaction counts** — fixed showing 0 (was using toInt on array, now uses array length)
- **Drag-to-scroll removed** — scroll via mouse wheel and scrollbar only
- **HTML stripping** — uses QTextDocument::toPlainText() instead of fragile regex
- **Composer focus** — reply button focuses the text input via focusProxy

## v0.13.1 (2026-03-29)

### Bidirectional Video Calls
- **Remote video display** — I420→QImage conversion via QPainter in CallDialog VideoWidget
- **Camera sends VP8** — matches Janus MCU (was H264), auto-negotiates caps (no hardcoded format/resolution/framerate)
- **Camera preview** — local VideoFrameProvider with 120x90 overlay at bottom-right of remote video
- **Dialog auto-resize** — expands to 400x500 when remote enables camera, shrinks to 300x340 when disabled
- **Remote avatar** — replaces video area when remote camera is muted
- **Dummy video track** — 16x16 black VP8 frame for audio-only calls (Janus MCU requires video from all publishers)
- **Skip dummy frames** — only show remote video when frames >32px

### Remote Media State Tracking
- **Mute/unmute signaling** — parse incoming mute/unmute messages, emit remoteMuteChanged signal
- **Mic muted indicator** — show muted icon on peer name when remote mic is muted
- **remoteVideoMuted/remoteAudioMuted** properties tracked on CallManager

### Call Dialog UX
- **Circular avatar** of remote party in header
- **circleButtonStyle()** helper for consistent button styling
- **Publisher ICE status** doesn't overwrite "Connected" when Active

### Code Review Fixes
- Ring timeout bypasses userActionReady (was stalling call forever)
- SubscribePipeline::onNewVideoSample uses QPointer guard (was use-after-free)
- leaveCallOnServer checks m_joinedCall (was double-DELETE on decline)
- STUN URL prefix replacement uses mid() (was corrupting mid-string matches)
- Video provider flags reset on provider change (was freezing after camera toggle)

### Code Simplification
- Extracted helpers: circleButtonStyle, indexOfToken, makeCandidateJson, callFlags, onAudioLevelUpdated
- Removed dead m_jpegDec code path and unused m_busWatchId
- Net -67 lines

## v0.13.0 (2026-03-28)

### Audio Calls — Fully Working with MCU/HPB
- **CallDialog** — QWidget-based call window with status breadcrumbs, mic/hangup/camera buttons
- **OPUS codec fix** — was MULTIOPUS, Janus rejected it
- **Accept/Decline** — setUserActionReady wiring for incoming call popup
- **Hangup** — DELETE /call with all=true to end call for both parties
- **Initial media state broadcast** — sent when remote peer joins (not just on ICE)
- **Stale call detection** — first conversation load seeds call state silently
- **Re-request subscriber stream** — when remote enables video mid-call

### Logging
- **TalqLog** — build-type-controlled logging system with file-based debug output
- Debug logs written to file for call troubleshooting

### Fixes
- Fixed crash on conversation select during active call
- Fixed call 404 errors from participant polling
- Fixed Janus codec config and participant polling that was breaking calls

## v0.12.2 (2026-03-28)

### UI Polish
- **Reply bar** — teal accent bar above composer shows "Author: preview..." with X to cancel
- **Context menu** — Telegram-style dark popup with emoji row, Download, Open in NC, Copy, Reply, Pin, Copy link, Thread (groups only), Delete (own + confirmation)
- **Emoji toggle** — clicking same emoji again removes it (POST 409 → DELETE)
- **Hover buttons fixed** — hit test at press time (hover state was clearing during click)
- **Call buttons** — subtle transparent icons with hover highlight + tooltips
- **Note to self** — bookmark icon in gray circle
- **Sidebar rows** — 62px compact height
- **Composer** — rounded pill input, dark background, larger buttons with hover
- **Welcome screen** — larger server card (420px), 14px fonts
- **Smooth avatars** — SmoothPixmapTransform on all painters
- **Splitter divider** — 1px line between sidebar and chat

### Bug Fixes
- **Code review fixes** — QDateTime include, screen() null guard, preview aspect clear, static regex
- **NC/Talk versions** — update on serverInfoChanged (was empty on first load)
- **Profile avatar** — reload on userInfoChanged
- **Thread action** — hidden in 1:1 chats

## v0.12.1 (2026-03-28)

### Fixes
- **Push + signaling not starting on session restore** — services only started on fresh login, not when restoring a saved session. Now starts on both paths.
- **Welcome screen status labels** — signaling and push status now update live when services connect/disconnect (was showing stale "disconnected" state).
- **Image click → preview viewer** — clicking image previews now opens a full-size viewer dialog.
- **Reaction clicks** — clicking reaction pills now toggles the reaction.

## v0.12.0 (2026-03-28)

### Full QWidget Conversion — QML Engine Eliminated
- **86% memory reduction** — 83MB private memory (was 600MB+ with QML)
- **Qt Quick, Qt QML, Qt QuickControls2 dependencies removed** — build: 38 targets (was 86)
- **QMainWindow** replaces QML ApplicationWindow — splitter layout, tray icon, shortcuts, geometry save/restore
- **ComposerWidget** — QTextEdit-based message input with send/attach buttons, font zoom
- **LoginWidget** — QWidget login form with server URL input
- **SettingsDialog** — 4-tab QDialog: Audio/Video, Notifications, General, Account
- **SidebarPainter** — QPainter conversation list with avatars, badges, search, squeeze mode
- **HeaderPainter** — QPainter chat header with avatar, call buttons, typing indicator
- **ThreadsPainter** — QPainter topics sidebar
- **Welcome screen** — logo, server info card, push/signaling status
- **User profile header** — avatar + display name + settings gear in sidebar

### Fixes
- **Missing messages after restart** — refreshLatest merge logic rewrote: dedup by ID, sort, reset model
- **Read status broken** — refreshLatest wasn't reading X-Chat-Last-Common-Read header
- **Image preview sizing** — uses actual aspect ratio from loaded image, compact placeholder until loaded
- **Empty message gaps** — skip messages with no text/file/reply for all senders
- **Shared avatar cache** — sidebar passes cached avatar to header (no duplicate HTTP fetch)
- **Camera dropdown empty** — deploy winks + mediafoundation GStreamer plugins
- **Font zoom scoped** — Ctrl+/- only affects chat messages + composer, not sidebar/header/settings
- **Code review fixes** — sendAction slot, QMenu leak, djb2 unsigned overflow, consolidated QSettings

## v0.11.0 (2026-03-27)

### ChatPainter — QPainter-based Message Renderer
- **Complete rewrite** of message rendering: replaced QML ListView + MessageBubble delegates with a single `QQuickPaintedItem` that renders all messages via QPainter
- **50% memory reduction** — ~385MB vs ~700MB+ with the old QML delegate approach
- **Zero delegate overhead** — only visible messages are painted (viewport culling), no QML item tree
- **Dynamic bubble widths** — message bubbles shrink to fit content, max 75% of chat width
- **Rich text** via QTextDocument with HTML links, mentions, word wrap
- **Async image loading** — avatars and file previews fetched via API, cached in memory, placeholder shown while loading
- **Inline image previews** — image attachments shown with rounded corners, click for full-screen preview
- **File attachment pills** — non-image files shown as rounded rect with document icon + filename
- **Reactions row** — emoji + count pills rendered inline
- **Clickable links** — cursor changes on hover, left-click opens in browser
- **Right-click context menu** — emoji quick-react + Copy/Reply/Delete actions, positioned at cursor with edge clamping
- **Hover action bar** — react (others) + reply (all) buttons appear on message hover
- **Emoji quick-react bar** — lightweight popup opens beside the smiley icon
- **Read status** — green filled circle (read) / empty circle (delivered) with cached lastCommonRead
- **Dark/light mode** — PainterTheme mirrors Theme.qml, auto-updates on toggle
- **Scroll** — mouse wheel + drag-to-scroll via QML MouseArea overlay
- **Date separators** — pill-style day headers between messages
- **System messages** — centered italic text for calls, joins, etc.
- **Message grouping** — consecutive same-author messages within 5 minutes share avatar/name

### Bug Fixes
- **Read status broken since v0.9.x** — `refreshLatest()` wasn't reading `X-Chat-Last-Common-Read` header. Fixed + cached in SQLite per conversation for instant status on app restart.
- **Empty green bars** — own messages with no text/file no longer render as tiny bubbles

### Cleanup
- Removed QML ListView + MessageBubble delegate path entirely
- MessageBubble.qml removed from QML module (file kept for reference)
- 130 lines of legacy toggle code removed

## v0.9.5 (2026-03-26)

### Video Call Fixes
- **Video renegotiation (m=video 0)** — enabling camera mid-call now works. Root cause: `enableCamera()` created two transceivers (one orphaned → `m=video 0`). Fixed with single transceiver approach in both PeerPipeline and PublishPipeline.
- **STUN URL format** — applied `stun:` → `stun://` conversion to PublishPipeline and SubscribePipeline (was only in PeerPipeline). STUN was silently failing in MCU mode.
- **Audio device selection** — mic and speaker settings from Settings dialog were silently ignored. PublishPipeline never passed `audioDeviceId`; `autoaudiosink` doesn't propagate `device` property. Fixed with explicit `wasapi2sink` → `wasapisink` → `directsoundsink` fallback chain.
- **Incoming call detection race** — overlapping conversation refreshes could both miss the `hasCall` false→true transition. Fixed with persistent `m_callState` map that survives across refresh cycles.

### Test Harness
- Video renegotiation test phase: enables camera mid-call, validates SDP has active `m=video` line
- `videotestsrc` support in PeerPipeline and PublishPipeline for headless testing

## v0.9.4 (2026-03-26)

### Major: BottomToTop ListView (conversation switch freeze fix)
- **Root cause**: `positionViewAtEnd()` / `positionViewAtIndex()` forced Qt to instantiate ALL MessageBubble delegates simultaneously, causing 1-4GB memory explosion and UI freeze on every conversation switch
- **Fix**: Reversed message storage to newest-first + `ListView.BottomToTop` layout. The view naturally starts at the bottom (newest messages visible) — no scroll calls needed
- Messages now stored newest-first in the model (`m_messages[0]` = newest)
- New messages from poller prepend at index 0 (appear at bottom)
- History loads append at end (appear at top on scroll-up)
- `cacheBuffer: 200` — only creates delegates near the viewport

### Memory & Stability Fixes
- **Poller backoff**: HTTP 401/403/404 stops polling; 5xx retries with exponential backoff (2s→60s)
- **Poller lastKnown:0 guard**: never start polling with lastKnownMessageId=0 (was downloading entire conversation history — 2000+ messages)
- **Message trimming**: cap at 200 messages per conversation, oldest trimmed on overflow
- **m_messageIds in postAndReplace**: prevents duplicate messages from poller
- **refreshLatest after file share**: sent files now appear immediately
- **cancelAll() safe iteration**: copies list before aborting
- **Cross-thread image providers**: FilePreviewProvider and AvatarProvider now moveToThread to main thread before network calls (eliminates "Cannot create children for a parent that is in a different thread" warnings)
- **MessageBubble anchors fix**: reply background Rectangle moved outside ColumnLayout (was causing 4000+ re-layout cycles)

### Other Fixes
- PushClient WebSocket error handler added
- PushClient auth failure reconnects instead of permanent dead state
- m_hasMoreHistory reset in setThreadId/setHideThreadMessages
- ConversationItem required property notificationLevel (eliminates "model is not defined" errors)
- markAsRead uses proper method on conversation open

## v0.9.3 (2026-03-26)

### Fixes
- **Duplicate message prevention** — `m_messageIds` updated in `postAndReplace()` and `deleteMessage()`, preventing poller from re-adding sent/deleted messages
- **Refresh after file share** — conversation refreshes immediately after uploading a file
- Poller diagnostic logging added

## v0.9.2 (2026-03-26)

### Call Fixes
- **MCU ICE candidate parsing** — fixed double-nested candidate extraction from Janus MCU signaling (was likely causing intermittent call failures)
- **STUN URL format** — convert Nextcloud's `stun:host:port` to `stun://host:port` for GStreamer compatibility
- **Test audio mode** — PeerPipeline supports `TALQ_TEST_AUDIO` env var for headless testing (audiotestsrc + fakesink)

### Automated Call Testing
- **talq-call-test.exe** — headless two-user MCU call test harness
- Authenticates kalin + test-talq, joins call via HPB, creates WebRTC pipelines, verifies ICE connection through real STUN/TURN, validates 3s stability, tears down cleanly
- Catches SDP issues, ICE failures, and signaling bugs without GUI

### Build & Deploy
- **deploy-dev.sh** — added audiotestsrc to GStreamer plugin list, fixed MSYS2 runtime DLL handling
- **Unified build paths** — all docs use `/c/build/talq` consistently

## v0.9.1 (2026-03-25)

### Room Avatars
- **Group chat avatars** — conversations without a specific user (group chats, public rooms) now show the room avatar fetched via authenticated OCS API
- **TqAvatar token fallback** — avatar component falls back to `image://avatar/room/{token}` when no userId is available

### Build & Deploy
- **deploy-dev.sh** — new script handles Qt + GStreamer DLL deployment after build, resolves MinGW runtime ABI conflict automatically
- **Unified build paths** — all docs and scripts now use `/c/build/talq` and `/c/src/talk-desktop-qt` consistently

### Fixes
- Triple-pass scrollToBottom for first-load delegate sizing

## v0.9.0 (2026-03-25)

### Memory Leak & Scroll Fix
- **Orphan network callbacks** — rapid conversation switching stacked duplicate refreshLatest() replies. Fixed: `m_refreshReply` member, cancelled on switch.
- **loadHistory() cascade** — `onContentYChanged` re-triggered after each prepend, loading entire history into memory. Fixed: 500ms debounce + `userHasScrolled` guard.
- **Unbounded FilePreviewProvider cache** — every preview (~3MB each) cached forever. Fixed: 50MB LRU eviction cap.
- **refreshLatest() ordering** — partition missing messages into older (prepend) / newer (append)
- **Persistent m_messageIds** — QSet replaces per-poll O(n) rebuild
- **onLastCommonReadChanged** scoped to changed range (was emitting for ALL messages every 15s)
- **scrollToBottom()** coalesced via 16ms timer
- **AvatarProvider** memory cache capped at 200 entries

### Video Calls — Compatibility
- **NC Talk video compatibility** — media state broadcasting, auto-camera detection, dual call buttons (audio/video)
- **SVG icon system** — geometric thin-stroke icons replace emoji throughout the app
- **Call status breadcrumbs** — shows signaling progress during connection (joining → signaling → connecting → active)
- **GStreamer plugin check** — disable call buttons when required plugins are missing
- **mfvideosrc** — use Media Foundation camera source instead of ksvideosrc
- **PLI keyframe fix** — request on src pad (not sink), periodic every 5s
- **Add-transceiver fix** — prevents m=video 0 in renegotiation SDP (untested)
- **Audio fallback chain** — autoaudiosrc/autoaudiosink with device selection override
- **MCU video renegotiation** — re-request subscriber streams after video accepted

### Fixes
- Duplicate "Reply" in context menu removed
- imageViewer.open() crash → downloadFile()
- Scroll-up no longer blocked by aggressive auto-scroll

## v0.8.3 (2026-03-23)

### Camera & P2P
- **Local camera preview** — PIP overlay in CallWindow via tee + appsink in PublishPipeline
- **P2P call mode** — direct peer-to-peer WebRTC for 1:1 calls (PeerPipeline), MCU used for group calls
- **Camera renegotiation** — enabling camera mid-call triggers proper SDP renegotiation

### Fixes
- Refresh latest messages from server after cache load (stale cache issue)
- Signal disconnect and QPointer guard in pipeline callbacks

## v0.8.2 (2026-03-22)

### Warm Carbon Design System
- **Phase 1: Theme foundation** — comprehensive dark/light theme with warm teal accent, standardized spacing/radius/font tokens
- **Phase 2: Component library** — 5 reusable QML components: TqAvatar, TqIconButton, TqBadge, TqSwitch, TqComboBox
- **Refactored UI** — replaced duplicated code across views with Tq* components

### Fixes
- Scroll-to-bottom catches late delegate height changes
- Negative window position restore fixed
- Reply bubble min width applied to other-person messages (was only own)
- TqIconButton: icon→iconText (AbstractButton.icon is FINAL)

## v0.8.1 (2026-03-22)

### Settings Dialog
- **Full settings dialog** — 4-tab layout (Audio & Video, Notifications, General, Account) with dark mode styling
- **Device persistence** — mic, speaker, and camera selections saved via QSettings, restored on app restart (matches by name, falls back to device ID for disambiguation)
- **Notification settings** — enable/disable, style (in-app popup vs Windows toast), sound mode (TalQ chime / system / none), all persisted
- **General settings** — start with Windows (registry auto-start), start minimized to tray, close to tray toggle
- **Account tab** — avatar, display name, server URL, Nextcloud/Talk versions, logout button
- **Click avatar to open** — click your avatar or display name in the sidebar header to open settings

### Per-Conversation Mute
- **Right-click mute** — right-click any conversation in the sidebar to Mute/Unmute
- **Server-synced** — uses Nextcloud Talk API (`/api/v4/room/{token}/notify`), persists across devices
- **Visual indicator** — "Muted" label shown on muted conversations

## v0.8.0 (2026-03-22)

### Video Calls
- **Receive remote video** — VP8 and H.264 codec auto-detection, decoded via GStreamer appsink to Qt VideoOutput. Tested working via MCU (Janus).
- **Send camera** — ksvideosrc (JPEG capture + jpegdec for HD) → openh264enc → rtph264pay, 1080p/720p with auto-fallback, toggle mid-call
- **SCTP data channel support** — required for MCU SDP compatibility (Janus includes datachannel m-line)
- **CallWindow video layout** — remote video fills window, controls auto-hide after 3s, camera toggle button, duration overlay
- **Settings** — camera device selection, video quality preset (Full HD / HD), persisted via Qt.labs.settings

### Call Polish
- **TURN server support** — credentials parsed from signaling settings, URL-encoded, configured on webrtcbin
- **Device selection wired to pipelines** — selected mic/speaker actually used (wasapi2src/wasapi2sink device property)
- **Incoming call decline fix** — leave room instead of call API (no more 404), m_joinedCall tracking
- **Auto-decline race fix** — m_userActionReady flag gates accept/decline on popup Component.onCompleted

### Chat UX
- **Scroll-to-bottom fixed** — no more jumping during image load, history prepend doesn't disrupt position
- **Image previews reserve height** — 200px placeholder prevents layout shift during async load
- **Reply bubble width** — own reply bubbles expand to fit quoted text (min 260px when quoting)
- **In-app image viewer** — click any image to view full-size in dark window (Esc to close)
- **Context menu** — Download and Open in Nextcloud items for file/image messages

### Dependencies
- Qt6::Multimedia added (QVideoSink, QVideoFrame, VideoOutput)
- GStreamer plugins: vpx, openh264, videoconvertscale, winks (camera)

## v0.7.1 (2026-03-21)

### Message Cache Fix
- Fixed file attachments (images, documents) disappearing after switching conversations
- Cache now stores original server JSON for lossless round-trip (previously reconstructed a lossy subset, dropping messageParameters)
- Mentions, thread metadata, and all parameter-dependent content now survive cache reload
- Schema v2 migration auto-purges stale entries on first launch

### Chat Scroll Stability
- Fixed chat jumping to older history during actions (image upload, footer changes)
- Replaced `onContentHeightChanged` auto-scroll with targeted `onCountChanged` handler
- Auto-scroll no longer disabled by programmatic scrolling (uses `onDraggingChanged` instead of `onMovingChanged`)
- Auto-scroll re-enables when user scrolls back to bottom
- Scroll-to-bottom button now correctly shows/hides based on position
- Scrollbar fades in during scrolling, fades out after 400ms of inactivity

### Text Selection
- Message text is now selectable with click-drag (Ctrl+C to copy)
- ListView switched to `interactive: false` with WheelHandler for mouse wheel scrolling

### File Handling Improvements
- Ctrl+V now handles files copied from Explorer (not just screenshots)
- Image files get preview confirmation; other files show file icon with name
- All file sends (paste, file dialog, drag-drop) show confirmation bar with caption field
- Files are never sent directly without user confirmation

## v0.6.1 (2026-03-20)

### Thread/Topic Fixes
- Fixed false thread detection in 1:1 chats — regular replies no longer create phantom topics
- Thread detection now uses the API-provided `isThread`/`threadId`/`threadTitle` fields instead of heuristic parent scanning
- "All Messages" renamed to "General" — now shows only non-thread messages (like Telegram's #General)
- Back arrow added to topics column header for returning to full chat list
- "+ New Topic" button moved from header to bottom of topic list to prevent overlap
- Sidebar can now be expanded/collapsed via toggle even when topics are active
- Draggable resize handles on sidebar and topic list dividers

### Scroll Fix
- Fixed scroll-to-bottom not working when opening conversations
- Replaced timer-based scroll with callback-driven approach using `onContentHeightChanged`

## v0.6.0 (2026-03-19)

### Threads / Topics (Telegram-style)
- 3-column layout for group chats with threads: squeezed sidebar icons | topics list | messages
- Sidebar auto-squeezes to 56px icon-only mode with smooth animation
- Manual squeeze toggle chevron at sidebar bottom
- Topics list with colored dots, unread badges, selection highlight
- "All Messages" row for unfiltered conversation view
- Inline topic creation: "+" button → type name → Enter
- Topic-aware chat header with color dot + group subtitle
- Dynamic composer placeholder: "Reply in [topic name]..."
- Thread index persisted in SQLite across app restarts
- Server capability check (requires Talk v22+ threads feature)
- Topic color palette centralized in Theme.topicColor()

### Upload Progress
- File upload shows progress bar in footer (filename + percentage)
- Animated progress indicator during WebDAV upload

### Paste Confirmation
- Ctrl+V no longer auto-sends images — shows preview bar first
- Caption field for adding text description before sending
- Send button (or Enter) to confirm, Escape or X to cancel

### Scroll to Bottom
- Floating ↓ button appears when scrolled up in chat history
- Click to jump to newest messages, teal hover highlight

### Fixes
- Typing indicator no longer leaks across conversations (room-scoped)
- Memory safety: QPointer guard in API callbacks, disconnect in setCache
- hasTopics flag preserved across conversation list refresh
- ThreadItem selection state properly bound in delegate
- thread_index cleared on conversation/account clear
- MessageCache wait timeout removed (prevents dangling lambdas)
- Deduplicated avatar images in squeezed ConversationItem
- Stale callback guard in ThreadListModel.fetchThreads
- refreshAfterCreate timer guards against conversation switch

---

## v0.5.3 (2026-03-19)

### Clipboard Paste
- Ctrl+V to paste screenshots and images directly into chat
- Clipboard images saved as temp PNG and sent via existing file upload pipeline

### User Status
- Online status heartbeat — TalQ now sets own status to "online" every 2 minutes
- Fixed status dots disappearing: dual-source status (user_status API primary, room API fallback)

### Context Menu Polish
- Layered shadow for depth (Telegram-style)
- Faster enter/exit transitions (100ms/60ms)
- Stronger emoji row separator and hover states
- Reduced emoji count to 6 to prevent overflow
- Quadrant-aware positioning with edge clamping

### Fixes
- Window no longer grows on every restart (geometry save guard during restore)
- Context menu no longer appears far above the cursor (correct popup height estimate)

---

## v0.5.2 (2026-03-19)

### Image Previews
- Authenticated inline image previews via `image://preview/` provider
- Thumbnails fetched from NC with auth headers, memory-cached per fileId
- Click image to open in Nextcloud browser

### Fixes
- Image attachments were invisible (preview disabled, pill excluded images)
- File shares now include read permission for recipients
- Deleted messages removed instantly from list
- Typing indicator filtered by current room
- Version bumped correctly across all files
- Auto-refresh disabled (push-only for real-time updates)

---

## v0.5.1 (2026-03-19)

### File Uploads
- 📎 button in composer for file picker
- Drag-and-drop files onto chat to upload
- WebDAV PUT upload + share to conversation

### Branding
- 123NET TalQ dedicated build (`cmake -DTALQ_BRAND=123NET`)
- Hardcoded server URL, no URL field on login
- Dual logos on splash (TalQ + brand), brand logo on welcome screen
- Separate Inno Setup installer for branded builds
- Self-signed code signing: 123 NET CPT (PTY) LTD

### Fixes
- Typing indicator filtered by current room (was showing wrong chat)
- Deleted messages removed instantly from list
- Window minimum width lowered to 500px
- Missing QML modules in installer (Qt.labs.settings, QtQuick.Dialogs)
- windeployqt used for complete dependency gathering

---

## v0.5.0 (2026-03-19)

### Real-Time Communication
- Push notifications via Nextcloud Notify Push WebSocket (instant message delivery)
- Typing indicators via standalone signaling (HPB) WebSocket
- Push status LED on avatar: green (push), amber (polling), red (disconnected)
- Signaling status on welcome screen

### Notifications & System Tray
- System tray icon with context menu (Show, Sound mode, Notifications, Quit)
- Sound modes: TalQ chime (default, bypasses DND), system sound, or none
- Tray icon badge with unread count (dynamically painted)
- Cross-chat notifications with sound even when window is focused
- Minimize to tray on close (X hides, tray Quit exits)
- Conversation list auto-refresh (30s fallback for push)

### File Attachments
- Inline display of shared files in messages
- Image files shown with preview placeholder (authenticated provider TODO)
- Other files shown as styled pills with MIME-type icons (video, audio, PDF, etc.)
- Click to open/download in browser

### Read Markers
- Mark conversation as read on open (POST /chat/{token}/read)
- Instant sidebar badge clear (local + server)
- Auto mark-as-read on new polled messages

### Font Zoom
- Ctrl+= zoom in, Ctrl+- zoom out, Ctrl+0 reset (0.7x to 2.0x)
- Scale persisted between sessions
- All font sizes scale via Theme.fontScale

### UX Polish
- Welcome screen: server info card with GridLayout alignment, push/signaling status
- Context menu: bigger icons (18px), aligned columns, taller rows (36px)
- Emoji bar positions inline with react button
- Hover action buttons: 32px with 20px icons
- Stronger hover backgrounds on menu items
- Reply scroll fix (delayed 100ms for footer resize)

### Stability Fixes
- Memory leak fix: broken beginInsertRows + unauthenticated image retries (5.5GB → 260MB)
- Conversation switch freeze fix: SQLite cache blocking main thread
- MessageCache moved to worker thread (fully async saves, non-blocking)
- Lazy-loaded Popups in MessageBubble (eliminates 100+ window handles)
- In-place conversation list updates (no beginResetModel during refresh)
- Participants/active join for signaling (prevents "not invited" error)
- Thread-safe messagesLoaded signal (double-invoke for main thread delivery)
- PushClient WebSocket state guard in authenticate callback
- Popup Loaders deactivated on close (memory cleanup)
- Tray icon pixmap cached (no disk reload per refresh)

### Other
- Exe renamed from talk-qt to talq
- Build timestamp in window title for debug tracking
- Code review findings addressed (8 fixes)

---

## v0.4.0 (2026-03-19)

### Threads / Topics
- Thread navigation for group conversations — topic list overlay with colored dots, reply counts, last activity
- Thread-aware message loading and polling with `threadId` filter
- "Reply in thread" context menu action
- ThreadItem and ThreadListView QML components
- Back navigation from thread to topic list

### Notifications & System Tray
- System tray icon with context menu (Show, Sound mode, Notifications, Quit)
- Windows toast notifications for new messages
- Sound modes: TalQ chime (default, bypasses DND), system sound, or none
- Cross-chat notifications — sound plays even when window is focused in another chat
- Auto-refresh conversation list every 10s for near-real-time unread detection
- Tray tooltip shows total unread count

### UX Polish
- Server info card on welcome page (server URL, NC version, Talk version, signaling)
- Clickable reaction pills with hover states (replaces plain text)
- Hover action button Y-clamping (no more clipping outside bubbles)
- Refined empty states with icons and helpful copy
- Context menu actions wired: Delete (API), Pin (API), Copy link, Reply in thread

### Window Management
- Splash screen centered on primary display, stays small (380x420)
- Reliable window state save/restore including maximized (QGroundControl pattern)
- Debounced geometry saving with `chatMode` guard
- Window grows to saved size/position after login

### Fixes
- Duplicate message prevention — poller vs POST response race condition resolved
- User agent shows "TalQ" without version during login
- Exe renamed from `talk-qt` to `talq`

---

## v0.3.0 (2026-03-19)

### Features
- SQLite message cache for instant conversation loading
- Avatar provider with 3-tier cache (memory, disk, server) and circular crop
- Dark/light mode toggle with warm teal accent (#2ec4b6)
- Emoji reactions with context menu and quick-react bar
- Reply-to with preview bar in composer
- Optimistic message sending with retry on failure
- Mention resolution in message text
- DWM dark title bar on Windows
- Inno Setup installer

### UI
- Hybrid message layout: flat for others, teal bubble for own messages
- Page-based ChatView with header/footer
- Connection status LED in sidebar header
- Window position/size persistence

---

## v0.2.0 (2026-03-18)

### Features
- Read receipts (sent/read indicators from X-Chat-Last-Common-Read)
- Date separators (Today/Yesterday/date pills)
- Custom app icon (blue chat bubble with Q)
- Splash screen with loading animation

### UI
- Complete UI redesign with refined dark theme
- Smooth hover/selection animations
- Conversation list: selection bar, search focus glow, thin scrollbar
- Send button with press animation

---

## v0.1.0 (2026-03-18)

### Initial Release
- Login Flow v2 (browser-based Nextcloud auth)
- Conversation list with unread badges, favorites, search
- Chat messaging with message bubbles
- Long-polling for live message updates
- Session persistence (auto-login on restart)
- Cross-compiled from Linux with mingw-w64

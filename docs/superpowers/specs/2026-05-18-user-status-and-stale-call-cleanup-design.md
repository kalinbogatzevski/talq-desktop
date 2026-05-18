# TalQ User Status + Stale-Call Cleanup — Design

Date: 2026-05-18
Status: Approved (brainstorming) — pending spec review
Target release: 0.29.10

## Problem

1. When TalQ crashed mid-call during testing, the user was left showing
   "In a call / Busy" on the Nextcloud server. There was no way inside TalQ
   to correct it.
2. TalQ has no user-status feature at all. The only `user_status` code is
   an ad-hoc `statusType:"online"` heartbeat in `main.cpp` set on login and
   every 2 minutes. There is no UI to set Away / DND / Invisible or a
   custom status message.

## Authoritative server findings (constraints — verified against bundled `spreed`)

Verified in `spreed/lib/Status/Listener.php` and
`spreed/lib/Controller/CallController.php::leaveCall`:

- The "📞 In a call / Busy" status is set and reverted **entirely
  server-side**, automatically, as a side effect of a participant's
  in-call flag transitioning (`BeforeParticipantModifiedEvent`). The
  client never sets or clears the `'call'` status itself. The server
  calls `setUserStatus(userId,'call',BUSY,true)` on join and
  `revertUserStatus(userId,'call',BUSY)` on leave.
- `DELETE /call/{token}` is **strictly bound to the requesting session**
  (`$this->participant->getSession()`). After a crash + relogin the client
  has a *new* session; the crashed session is a separate participant row
  the server still counts as in-call. A non-moderator client has **no API
  to force-drop another (dead) session**. That stale call participant is
  cleared only by the server's session ping-timeout.

Consequences for scope:

- The visible stuck "Busy/In a call" **can** be corrected instantly and
  reliably by the client via the Nextcloud user_status app endpoint
  `DELETE …/user_status/revert/call`, which restores the pre-call backup.
- The room-level call badge from a *crashed* session self-heals only on
  the server ping-timeout. TalQ will not pretend to force this; it focuses
  on (a) instant status correction and (b) preventing the leak on all
  clean exits.

## Goals

- A real user-status feature with full Nextcloud parity: 4 status types
  (Online / Away / Do not disturb / Invisible), emoji + free-text custom
  message, server-provided predefined presets, and a "clear after" timer.
- Instant correction of a crash-stuck call status on login.
- Prevent the stale-call leak on every clean exit (window close, logout,
  quit).
- Consistent with TalQ design language ("calm, warm, fast" + Mission
  Control) and the existing `core/` manager separation of concerns.
- Must never crash or block (TalQ "must not die" rule).

## Non-goals / out of scope

- Forcing the server to drop a crashed foreign session early (no client
  API exists for non-moderators).
- Showing/altering *other* users' statuses (TalQ already paints contacts'
  presence dots from the conversation list; unchanged here).
- Moderator "end call for everyone" flows.

## Architecture (Approach 1: dedicated core manager)

### `core/UserStatusManager.{h,cpp}` (new)

Constructed with `ApiClient*` + `AuthManager*` (same pattern as
`CallManager`). Owns all user-status state and the Nextcloud `user_status`
OCS v1 protocol.

State:
- `Status status` enum: `Online, Away, Dnd, Invisible, Offline`
- `QString message`, `QString icon` (emoji), `QString messageId` (non-empty
  if a predefined preset is active)
- `qint64 clearAt` (unix seconds; 0 = none)
- `bool statusIsUserDefined`
- `QVector<PredefinedStatus> predefined` (cached; `{id, icon, message,
  clearAtSpec}`)

Public methods:
- `void onLoggedIn()` — see Data flow.
- `void setStatusType(Status)` → `PUT user_status/status {statusType}`
- `void setPredefined(const QString &messageId, qint64 clearAt)` →
  `PUT user_status/message/predefined {messageId, clearAt?}`
- `void setCustom(const QString &icon, const QString &text, qint64 clearAt)`
  → `PUT user_status/message/custom {statusIcon?, message, clearAt?}`
- `void clearMessage()` → `DELETE user_status/message`
- Getters for all state; `const QVector<PredefinedStatus>& predefinedStatuses()`

Signals: `statusChanged()`, `predefinedLoaded()`, `error(QString)`.

Endpoints use the existing `ApiClient` (`get`, `getArray`, `put`, `del`
already build the `/ocs/v2.php/<path>` URL and accept JSON bodies). No new
`ApiClient` methods. OCS paths used:
- `GET  apps/user_status/api/v1/user_status`
- `GET  apps/user_status/api/v1/predefined_statuses`
- `PUT  apps/user_status/api/v1/user_status/status`
- `PUT  apps/user_status/api/v1/user_status/message/predefined`
- `PUT  apps/user_status/api/v1/user_status/message/custom`
- `DELETE apps/user_status/api/v1/user_status/message`
- `DELETE apps/user_status/api/v1/user_status/revert/call`

### `ui/StatusPopover.{h,cpp}` (new)

Frameless `QWidget` popup, same construction/teardown/stylesheet pattern as
the existing `ui/EmojiPickerWidget`. Sections:
- Four status-type rows with the presence-dot colors + labels.
- Preset list (icon + message) from `predefinedStatuses()`.
- Custom-message row: emoji button (reuses `EmojiPickerWidget`) + text
  field.
- "Clear after" dropdown: 30 min / 1 hour / 4 hours / today / this week /
  Don't clear → mapped to a `clearAt` unix timestamp (Don't clear = 0).
- "Clear status" action (calls `clearMessage()` and resets type to Online
  if appropriate).

Talks only to `UserStatusManager`. Optimistic: a selection updates the UI
immediately; failure rolls back and shows a one-line inline error.

### Sidebar profile bar (`MainWindow::buildChatPage`, `SidebarPainter`)

- Own-avatar status dot using the existing presence-dot color logic
  (`SidebarPainter.cpp` ~L466 / ~L674).
- "● Online ▾" pill next to `m_profileNameLabel` (glanceable readout).
- Avatar dot or pill click → open `StatusPopover` anchored to the profile
  bar. Avatar-click currently opens Settings; Settings is moved onto an
  explicit gear control in the profile bar (added if one is not already
  present) so the avatar/pill is free for status.

### `main.cpp`

Remove the ad-hoc 2-min "online" heartbeat + the on-login status PUT.
`UserStatusManager` owns the heartbeat and only re-asserts `online` when
`status == Online && !statusIsUserDefined`, so it never overwrites a
user-set Away/DND/Invisible or a custom message. Wire
`AuthManager::loggedInChanged`/`restoringChanged` → `onLoggedIn()`.

## Data flow

On login (`AuthManager` logged-in/restored → `UserStatusManager::onLoggedIn`):
1. `GET predefined_statuses` → cache → `emit predefinedLoaded`.
2. `GET user_status` → populate → `emit statusChanged` (sidebar repaints).
3. `DELETE user_status/revert/call` → on success re-`GET user_status` →
   `statusChanged`. Harmless if nothing was stuck.

User changes status (popover): snapshot prior state → optimistic local
update + `statusChanged` → fire matching `PUT`/`DELETE` → on success
reconcile with server echo; on failure restore snapshot + `emit error`.
One in-flight write at a time; newest selection supersedes (no queue).

Heartbeat: 2-min timer in `UserStatusManager`; re-asserts `online` only
when `status == Online && !statusIsUserDefined`.

Display mapping (shared with contacts' dots): Online → green, Away →
amber, DND → red (minus glyph), Invisible → hollow grey, Offline → grey.

## Stale-call cleanup

- Status half (instant, reliable): the login `revert/call` above.
- Prevention half: ensure `CallManager::teardown → leaveCallOnServer`
  runs on every clean exit — window close mid-call, `AuthManager::logout()`,
  and app quit (`QCoreApplication::aboutToQuit`). Today it only fires on
  explicit hang-up/teardown; add a guarded best-effort leave on those
  paths. A crashed session's room badge still self-heals only on the
  server ping-timeout (documented limitation, not worked around).

## Error handling

Every `user_status` call is non-fatal: failure rolls back the optimistic
UI and shows a one-line inline message in the popover; never blocks, never
crashes. Offline/unreachable: popover opens read-only with "Can't reach
server". `revert/call` failure is logged at `qInfo` and ignored
(best-effort). Consistent with TalQ's "must not die" rule.

## Testing

`UserStatusManager` is pure logic over `ApiClient` (headless-exercisable).
Given TalQ's light test infra, validation is a focused manual matrix plus
`qInfo` lifecycle logging so a real run is self-diagnosing:

- Set each status type; verify dot + pill + server echo.
- Custom message + emoji + each "clear after"; verify `clearAt`.
- Apply a predefined preset; clear message.
- Login while server shows a stuck `call` status → auto-reverts to prior.
- Offline: optimistic change rolls back, inline error shown, no crash.
- Heartbeat does not stomp a user-set Away/custom message.
- Clean exits (window close mid-call, logout, quit) leave the call on the
  server.

## Files

New: `src/core/UserStatusManager.h`, `src/core/UserStatusManager.cpp`,
`src/ui/StatusPopover.h`, `src/ui/StatusPopover.cpp`.
Modified: `src/main.cpp` (wire manager, drop ad-hoc heartbeat),
`src/ui/MainWindow.cpp` (profile-bar dot + pill, open popover, gear↔settings),
`src/painter/SidebarPainter.{h,cpp}` (own-status dot/pill paint hook),
`src/core/CallManager.cpp` (best-effort leave on clean exits),
`CMakeLists.txt` (new sources), `CHANGELOG.md`, `installer/talq-setup.iss`
+ `CMakeLists.txt` version bump to 0.29.10.

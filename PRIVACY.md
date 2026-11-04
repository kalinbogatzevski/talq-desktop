# Privacy Policy

**Short version: TalQ collects no personal data about you. There is no
telemetry, no analytics, and no crash reporting. Nothing about your usage is
sent to the TalQ developers.**

TalQ is a desktop client for [Nextcloud Talk](https://nextcloud.com/talk/). It
is a tool you point at a server you (or your organization) control. The
developers of TalQ do not operate a backend service for it and do not receive
your messages, calls, contacts, account details, or usage statistics.

## What data TalQ handles, and where it goes

- **Your Nextcloud server.** TalQ connects only to the Nextcloud server address
  you enter at sign-in. Your messages, calls, files, and account information are
  exchanged with **that server**, and are governed by **that server's** privacy
  policy and your agreement with whoever operates it — not by TalQ. The TalQ
  developers never see this traffic.

- **Link previews.** When a message contains a web address, TalQ asks **your
  Nextcloud server** (its built-in link-preview service, the one Talk's own web
  client uses) for the page's title, description and picture, and shows them
  under the message. The linked site is contacted by your server, not by your
  computer, so it does not see your IP address; the address itself goes to your
  server for this, like the message it came in. Nothing goes to the TalQ
  developers. Turn it off in **Settings → Link previews**.

- **Chat-list folders.** The folders you make in the chat list are kept per
  account in a small file in TalQ's data folder **on your computer**. Nothing
  about them leaves your computer unless you turn on folder sync (below), which
  is off by default.

- **Folder sync (off by default).** With **Settings → Keep my folders in sync**
  on, TalQ also keeps your folders in one small hidden file, `.talq-folders.json`,
  at the top of **your own Nextcloud Files**, over the connection and sign-in it
  already uses. The file holds only the folders: their names, their order, which
  chats are in each (by room code, never a chat's name or messages) and whether
  each folder, and the "Other" section, is open or closed. It is never sent to the
  TalQ developers or anyone else.
  While sync is on, TalQ reads the file now and then (about every ten minutes,
  also from the tray) and writes it when your folders change. Before it syncs on
  a computer, and again whenever that check has to be repeated, TalQ makes a tiny
  test file (`.talq-folders.probe-` and a random ending, holding only `{}`) to
  check that your server can store the file safely, and normally deletes it
  right away; if the check is interrupted (sync turned off, signed out, TalQ
  closed, or the sign-in refused) or the deletion fails, it can stay. Your
  server's trash or file versions may keep copies of either file. On your
  computer, sync keeps a note of the last state it synced, a marker that the
  check passed, and one copy of your folders file from just before a sync last
  removed anything. Turning sync off stops syncing and leaves the file in your
  Files; to remove it, turn sync off on every computer, then delete
  `.talq-folders.json` in your Files (show hidden files first).

- **Update check.** TalQ periodically asks GitHub whether a newer version exists
  by fetching a public release manifest from
  `https://api.github.com/repos/kalinbogatzevski/talq-desktop`. This is an
  anonymous request: TalQ sends no identifying information. As with any web
  request, GitHub (the host) may log standard connection metadata such as your
  IP address and a generic application user-agent; this is handled under
  [GitHub's privacy statement](https://docs.github.com/site-policy/privacy-policies/github-general-privacy-statement),
  and the TalQ developers receive none of it.

- **On-device only.** Your login credentials / session token, your settings, any
  cached chat history and avatars, and the local debug log (`talq_debug.log`)
  are stored **on your computer**. They are never uploaded by TalQ (the one
  exception is opt-in: your chat-list folders, to your own Nextcloud Files, when
  you turn on folder sync, above). The optional
  "Collect diagnostics" feature writes a `.txt` file locally for you to inspect
  or share manually — TalQ does not transmit it anywhere on its own.

## What TalQ does NOT do

- No analytics, telemetry, or usage tracking.
- No advertising or third-party trackers.
- No automatic crash/error reporting to the developers.
- No selling or sharing of any data (there is none to sell or share).

## Branded builds

An organisation may distribute its own branded build of TalQ to its users. A
branded build is the **same open-source application**, differing only in
pre-configured Nextcloud server endpoints and branding. It collects no
additional data; the same statements above apply.

Data you exchange with the Nextcloud server you connect to is governed by that
server operator's own privacy terms, not by this policy — which covers the
application itself. If you received TalQ from an organisation rather than from
this repository, ask them for their server-side terms.

## Changes

If TalQ ever adds a feature that transmits data to the developers (for example,
opt-in crash reporting), this document will be updated before that feature
ships, and any such collection will be opt-in.

## Contact

Questions: open an issue at
<https://github.com/kalinbogatzevski/talq-desktop/issues>.

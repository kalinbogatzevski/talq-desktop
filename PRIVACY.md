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
  are stored **on your computer**. They are never uploaded by TalQ. The optional
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

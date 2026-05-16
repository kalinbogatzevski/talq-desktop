# Third-Party Notices

TalQ is licensed under the Apache License 2.0 (see `LICENSE`). It links
against and/or bundles the third-party components listed below, each under
its own license. This file is distributed with the application to satisfy
those attribution requirements.

---

## Qt 6

- **License:** GNU Lesser General Public License v3.0 (LGPL-3.0)
- **Copyright:** © The Qt Company Ltd. and other contributors
- **Modules used:** Core, Gui, Widgets, Network, Sql, WebSockets, Svg,
  Concurrent, Multimedia
- **Usage:** Dynamically linked (shared libraries). Qt is not modified.
  As required by the LGPL, Qt is shipped as separate shared libraries so
  the user can replace them with a compatible build.
- <https://www.qt.io/licensing/> · <https://www.gnu.org/licenses/lgpl-3.0.html>

## GStreamer and bundled plugins

- **License:** GNU Lesser General Public License v2.1 (LGPL-2.1) for the
  GStreamer core, base/good libraries and most bundled plugins.
- **Copyright:** © The GStreamer team and contributors
- **Usage:** Dynamically linked; plugins shipped as separate, replaceable
  shared libraries.
- Bundled plugin/codec libraries and their licenses:
  - `libvpx` (VP8/VP9) — BSD-3-Clause, © The WebM Project / Google
  - `openh264` — BSD-2-Clause; the OpenH264 binary is subject to Cisco's
    royalty-bearing AVC/H.264 patent terms. See
    <https://www.openh264.org/BINARY_LICENSE.txt>
  - `libopus` — BSD-3-Clause, © Xiph.Org Foundation
  - `libnice` — LGPL-2.1 / MPL-1.1, © Collabora, Nokia and contributors
  - `libsrtp2` — BSD-3-Clause, © Cisco Systems, Inc.
  - `libwebrtc-audio-processing` (webrtcdsp) — BSD-3-Clause, © The WebRTC
    project authors
  - OpenSSL — Apache License 2.0, © The OpenSSL Project
- <https://gstreamer.freedesktop.org/documentation/frequently-asked-questions/licensing.html>

## Twemoji

- **Emoji graphics license:** Creative Commons Attribution 4.0
  International (CC-BY 4.0)
- **Code/data license:** MIT
- **Copyright:** © 2020 Twitter, Inc. and other contributors; the project
  is now community-maintained.
- **Attribution (required):** This product uses the Twemoji emoji set.
  Twemoji graphics are licensed under CC-BY 4.0
  (<https://creativecommons.org/licenses/by/4.0/>). Source:
  <https://github.com/jdecked/twemoji>
- Bundled under `resources/twemoji/`.

## Inter typeface

- **License:** SIL Open Font License, Version 1.1 (OFL-1.1)
- **Copyright:** © Rasmus Andersson. "Inter" is a Reserved Font Name
  under the OFL.
- **Usage:** The font file `resources/fonts/Inter.ttf` is bundled and
  embedded. Redistribution is permitted under the OFL; the font is not
  sold on its own and the Reserved Font Name is not used for modified
  versions.
- <https://github.com/rsms/inter> · <https://openfontlicense.org/>

## Feather Icons (`resources/icons/`)

- **License:** MIT
- **Copyright:** © 2013–2017 Cole Bemis
- The SVG glyphs in `resources/icons/` are derived from Feather Icons
  (`mic`, `mic-off`, `phone-call`, `phone-off`; `react` from `smile`;
  `reply` from `corner-up-left`), lightly modified (color/stroke width).
- Source: <https://github.com/feathericons/feather>
- MIT License:

  > Permission is hereby granted, free of charge, to any person obtaining
  > a copy of this software and associated documentation files (the
  > "Software"), to deal in the Software without restriction, including
  > without limitation the rights to use, copy, modify, merge, publish,
  > distribute, sublicense, and/or sell copies of the Software, and to
  > permit persons to whom the Software is furnished to do so, subject to
  > the following conditions:
  >
  > The above copyright notice and this permission notice shall be
  > included in all copies or substantial portions of the Software.
  >
  > THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND.

## System fonts (not bundled)

- "Segoe Fluent Icons" / "Segoe MDL2 Assets" / "Segoe UI Symbol" are
  referenced only as font-family fallbacks for symbol glyphs. These
  Microsoft fonts are **not** bundled or redistributed; the application
  relies on the copy provided by the operating system.

---

### Trademarks

"Nextcloud" is a registered trademark of Nextcloud GmbH. TalQ is an
independent, unofficial client and is not affiliated with or endorsed by
Nextcloud GmbH. Other names may be trademarks of their respective owners.

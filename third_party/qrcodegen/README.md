# qrcodegen — QR Code generator library (C++)

Draws the QR code in "TalQ for your phone" (Settings > Account), the link a phone
camera opens to install the Android app.

- Upstream: <https://github.com/nayuki/QR-Code-generator>, tag `v1.8.0`, files
  `cpp/qrcodegen.hpp` and `cpp/qrcodegen.cpp`.
- **Unmodified.** Do not edit these two files; replace them with a newer tag.
  SHA-256 of the upstream bytes (LF line endings; a CRLF checkout differs):
  - `qrcodegen.hpp` b779c3b156cf7a57ce789d6fee4fc991ccc2913774d26c909d22bb8f26b2a793
  - `qrcodegen.cpp` 1f3b3fcdac6954c32cf583ccd02ec9b5901f756a38c461acedc70be4a77d3757
- License: MIT, © Project Nayuki (in the header of each file, and in
  `THIRD-PARTY-NOTICES.md`).
- Built as the static library `talq-qrcodegen` (`CMakeLists.txt`). TalQ code reaches it
  only through `src/core/PhoneAppCode.h` / `src/core/PhoneAppCode.cpp`.

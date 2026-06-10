# Code Signing Policy

> **DRAFT** — review/adjust the team members and roles below before submitting
> the SignPath Foundation application.

TalQ's public (generic) Windows builds are code-signed through the
[SignPath Foundation](https://signpath.org/)'s free signing program for
open-source projects. SignPath verifies that each signed binary is an
**automated build produced from this public repository** (origin/provenance
verification: repository, branch, build agent and configuration).

> Free code signing provided by [SignPath.io](https://signpath.io/), certificate
> by [SignPath Foundation](https://signpath.org/).

## Scope

- **Signed:** the generic `TalQ-v<version>-Setup.exe` installer and the
  `talq.exe` it installs, built in CI (GitHub Actions) from this repository's
  `main` branch.
- **Not signed by this program:** the `123NET` build. It is the **same
  open-source code**, only pre-configured with the 123NET Nextcloud servers and
  branding — but that server config + branding are deliberately kept out of the
  public repository (the public mirror is secret/host-gated), so this build
  cannot be provenance-verified against the public source and is out of scope
  for SignPath. It is signed separately (or distributed with a Defender
  allow-list to its small user group).

## Team & roles

| Role | Trust | Member(s) |
|------|-------|-----------|
| **Author** | May commit source without prior review | Kalin Bogatzevski (`@kalinbogatzevski`) |
| **Reviewer** | Reviews all changes from non-committers | Kalin Bogatzevski (`@kalinbogatzevski`) |
| **Approver** | Authorizes each signing request | Kalin Bogatzevski (`@kalinbogatzevski`) |

All team members use **multi-factor authentication** for both GitHub and
SignPath.io.

## Build & signing flow

1. A tagged release on `main` triggers the GitHub Actions release workflow,
   which builds the generic installer from source (Qt + MinGW + GStreamer +
   Inno Setup).
2. The workflow submits the built artifact to SignPath.io.
3. An Approver authorizes the signing request; SignPath signs with the
   Foundation certificate (key held in SignPath's HSM) after verifying build
   provenance.
4. The signed installer is published to the GitHub release.

## Privacy

Code signing transfers no user data. SignPath receives only the build artifact
and its provenance metadata for verification and signing.

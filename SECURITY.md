# Security policy

## Supported versions

Wisp is released from `main`; each release is a `vX.Y.Z` tag. Security fixes go into the next release, and only the latest release is supported. Update to it before reporting, if you can.

## Reporting a vulnerability

**Do not open a public issue.** Report it privately through GitHub's [private vulnerability reporting](https://github.com/ShouChenICU/Wisp/security/advisories/new) (the repository's "Security" tab, "Report a vulnerability"). Only the maintainer can see the report.

Please include:

- the firmware version (the admin console's System page, or `fw` in `WISP_BOOT_REPORT`) and the board, or the simulator;
- what an attacker can do, and from where (the LAN, the setup hotspot, a model or MCP server's reply, a scene, the SD card);
- the steps to reproduce it, and a proof of concept if you have one.

Leave real API keys, Wi-Fi passwords and personal data out of the report.

Wisp is maintained by one person, so replies are on a best-effort basis. You will hear back once the report has been looked at, and again when a fix is released. Please keep the details private until then.

## Scope

In scope: the firmware and the simulator in this repository, the admin console and its REST/WebSocket API, the scene sandbox (Lua, its file, HTTP and device permissions), the agent's tools, and pairing and API-key handling (keys are write-only and must never be readable back).

Out of scope: the model, ASR, TTS and MCP services you configure, and attacks that need physical access to an unlocked device or its USB serial console.

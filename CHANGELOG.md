# Changelog

## RBTV+ port — native catalogue screen

- Made the native RBTV+ screen the default app view (the desktop preview fixture route remains unchanged).
- Added native sport selection, a live catalogue list, match detail and real stream choices using the new API client.
- Added editable HTTPS data/origin settings and a separate approval confirmation. Changing an endpoint revokes approval; saving values alone does not make a request.
- Routed resolved direct HTTP(S) stream URLs and returned request headers to the existing native player.
- Shows a clear unsupported-source message when RBTV+ resolves a source without a direct URL, because the Android P2P/CSL/WebRTC/CDN middleware is not ported.
- No service endpoint has been contacted during development, no demo match catalogue was added, and console playback remains unverified.

## RBTV+ port — native protocol layer

- Added a bounded Protocol Buffers decoder for RBTV match, league, team, stream, signature and user-region payloads.
- Added the dynamic MD5-prefix builder and a native API client for bootstrap, live catalogue, match details and stream resolution.
- Required explicit HTTPS endpoints and kept the default API endpoint unset. Network calls happen only when the caller invokes them.
- Added offline parser/signature unit tests and a documented test runner.
- The API layer is not yet connected to the native catalogue/detail UI; no endpoint has been contacted and live playback is not validated.

## RBTV+ port — repository setup

- Established RB-Plus as the working repository for the RBTV+ PS5 port.
- Documented that the v0.3.0 ZIP is a Windows-hosted prototype, not the native PS5 integration.
- Recorded the current integration boundary: native RBTV+ catalogue, match detail and stream negotiation are still pending.
- Preserved inherited Stremio Plus source history and license notices while the native port is adapted.

## 0.5.6 — 2026-10-08 (upstream Stremio Plus baseline)

The inherited Stremio Plus 0.5.6 baseline includes the changes summarized below.

- Choose a custom download folder across accessible storage.
- Move existing downloads with recoverable interrupted-move handling.
- Improve download checkpoint and full-storage recovery.
- Remove the old private-volume installation reservation.
- Add configurable next-episode autoplay and countdown.
- Refine the launch/loading screen.

For the full inherited changelog, see the upstream repository history. These entries describe the starting native codebase, not completed RBTV+ functionality.

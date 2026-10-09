# Changelog

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

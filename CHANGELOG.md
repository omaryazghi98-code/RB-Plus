# Changelog

## 0.5.6 — 2026-10-08

Stremio Plus 0.5.6 gives you more control over your downloads and makes it easier to keep watching your favorite series.

### What's new

- **Choose your download folder.** Browse internal, M.2 and external storage from Settings, or create a "Stremio Plus Downloads" folder. The app applies and verifies `0777` permissions and checks that the folder is writable.
- **Move your existing downloads.** Changing folders moves completed videos, partial downloads, queued items, artwork and saved progress together. Transfers pause during the move, and interrupted moves can be recovered.
- **Better download recovery.** Improved handling of checkpoint failures and full disks. Existing files remain visible when their folder is readable, including unrecognized downloads that you can remove to recover space.
- **Fixed excessive installation storage.** Removed the 16 GiB private storage reservation requested by earlier builds. Streaming caches grow only as data is written.
- **Watch the next episode.** A new overlay shows its thumbnail, title and episode number, with "Watch now" and "Ignore" controls. The countdown defaults to 15 seconds and can be adjusted or disabled in Settings. Circle dismisses it.
- **Refined the loading screen.** Reduced the title logo to half its previous width and height while preserving its proportions.

### Upgrading

An old installation may retain the 16 GiB reservation after its files are replaced. To reclaim it, uninstall the registered app through the console or loader, then install this version. Keep `/data/Stremio` and your downloaded-video folders. You may need to sign in again.

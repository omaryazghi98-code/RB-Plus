// Stremio native startup storage. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

struct Ps5StoragePaths {
    const char* app = "/app0";
    const char* data = "/download0/stremio";
    const char* logs = "/data/Stremio";
    int filesystem_status = 0;
    bool helper_requested = false;
    bool filesystem_available = false;
    bool logs_available = false;
};

// Called once while main still has no application workers. Requests the same
// filesystem capability as the supplied ProsperoLight reference when needed.
Ps5StoragePaths ps5_prepare_storage() noexcept;

// Fixed startup stages only: no URLs, account data or arbitrary library text.
// These small unbuffered receipts precede the full asynchronous diagnostics.
void ps5_boot_note(const char* stage) noexcept;
void ps5_boot_close() noexcept;

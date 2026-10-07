// Stremio - Actual PS5 Pad light API with controlled libScePad boundaries.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#include "platform/ps5/pad.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
int checks = 0, opens = 0, closes = 0, sets = 0, resets = 0, logs = 0;
int set_result = 0, reset_result = 0, connection_count = 1;
bool connected = true, intercepted = false, wrong_handle = false;
std::array<std::uint8_t, 4> last_color{};
std::vector<std::string> operations;

void expect(bool value, const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}

void pad_light() {
    hui::ps5::Pad pad;
    expect(pad.open() && opens == 1, "native Pad opens exactly one controller handle");
    expect(pad.set_light_bar(30, 80, 140) && sets == 1 && last_color == std::array<std::uint8_t, 4>{30, 80, 140, 0},
           "RGB is passed through the real Pad adapter with the existing four-byte ABI");
    for (int i = 0; i < 100; ++i) pad.set_light_bar(30, 80, 140);
    expect(sets == 1, "identical colors never spam scePadSetLightBar");
    set_result = -1;
    expect(!pad.set_light_bar(130, 50, 90) && sets == 2 && logs == 1, "a native failure returns false and logs once");
    for (int i = 0; i < 100; ++i) pad.set_light_bar(std::uint8_t(i), 50, 90);
    expect(sets == 2, "changing target RGB cannot bypass the native error cooldown");
    pad.tick(1.9f);
    expect(!pad.set_light_bar(130, 50, 90) && sets == 2, "retry remains blocked before two seconds");
    pad.tick(0.11f);
    expect(!pad.set_light_bar(130, 50, 90) && sets == 3 && logs == 1, "persistent native errors retry without repeating logs");
    set_result = 0; pad.tick(2.1f);
    expect(pad.set_light_bar(130, 50, 90) && sets == 4, "transient native errors recover after cooldown");

    std::array<hui::PadSample, 64> samples{};
    intercepted = true;
    expect(pad.read(samples) == 1 && (samples[0].buttons & hui::pad_bits::kIntercepted),
           "light handling preserves intercepted input samples");
    const int before_intercept = sets;
    expect(!pad.set_light_bar(130, 50, 90) && sets == before_intercept, "the title does not write the light while the PS overlay owns the pad");
    intercepted = false; pad.read(samples);
    expect(pad.set_light_bar(130, 50, 90) && sets == before_intercept + 1,
           "returning from the PS overlay reapplies even an unchanged desired color");
    connected = false; pad.read(samples);
    const int before_disconnect = sets;
    expect(!pad.set_light_bar(20, 40, 60) && sets == before_disconnect, "a disconnected controller receives no light commands");
    connected = true; ++connection_count; pad.read(samples);
    expect(pad.set_light_bar(130, 50, 90) && sets == before_disconnect + 1,
           "reconnecting restores the title color on the same open pad handle");
    expect(opens == 1, "light changes and reconnect handling never open a second controller");
    expect(pad.reset_light_bar() && resets == 1, "reset invokes scePadResetLightBar for the existing handle");
    expect(pad.reset_light_bar() && resets == 1, "a repeated reset with no override is harmless");
    expect(pad.set_light_bar(130, 50, 90), "a new title can recolor after restoring the system default");
    reset_result = -2;
    const int before_reset_logs = logs;
    expect(!pad.reset_light_bar() && resets == 2 && logs == before_reset_logs + 1, "reset errors are reported without closing playback input");
    expect(!pad.reset_light_bar() && logs == before_reset_logs + 1, "reset error logging is also bounded");
    reset_result = 0;
    operations.clear();
    pad.close();
    const auto reset = std::find(operations.begin(), operations.end(), "reset");
    const auto close = std::find(operations.begin(), operations.end(), "close");
    expect(reset != operations.end() && close != operations.end() && reset < close && closes == 1,
           "normal exit restores the system color before closing the same pad");
    expect(!pad.set_light_bar(1, 2, 3), "a closed pad refuses later color updates");
    pad.close();
    expect(closes == 1 && !wrong_handle, "reset and close are idempotent and never touch another handle");
}
} // namespace

extern "C" {
int sceUserServiceInitialize(const void*) { return 0; }
int sceUserServiceTerminate() { return 0; }
int sceUserServiceGetInitialUser(int* user) { *user = 99; return 0; }
int scePadInit() { return 0; }
int scePadOpen(int user, int type, int index, const void*) {
    ++opens; wrong_handle = wrong_handle || user != 99 || type != 0 || index != 0; return 17;
}
int scePadRead(int handle, void* out, int count) {
    wrong_handle = wrong_handle || handle != 17;
    if (count < 1) return 0;
    auto* bytes = static_cast<unsigned char*>(out);
    std::memset(bytes, 0, 120);
    const std::uint32_t buttons = intercepted ? hui::pad_bits::kIntercepted : 0;
    const std::int32_t connection = connected ? 1 : 0;
    std::memcpy(bytes, &buttons, 4); std::memcpy(bytes + 0x4c, &connection, 4);
    bytes[0x68] = std::uint8_t(connection_count);
    return 1;
}
int scePadClose(int handle) { wrong_handle = wrong_handle || handle != 17; ++closes; operations.emplace_back("close"); return 0; }
int scePadSetVibrationMode(int handle, int mode) { wrong_handle = wrong_handle || handle != 17 || mode != 2; return 0; }
int scePadSetVibration(int handle, const void*) { wrong_handle = wrong_handle || handle != 17; return 0; }
int scePadSetLightBar(int handle, const void* color) {
    wrong_handle = wrong_handle || handle != 17; ++sets;
    std::memcpy(last_color.data(), color, 4); operations.emplace_back("set"); return set_result;
}
int scePadResetLightBar(int handle) {
    wrong_handle = wrong_handle || handle != 17; ++resets; operations.emplace_back("reset"); return reset_result;
}
}

namespace hui::sys {
void log(const char* format, ...) { if (std::strstr(format, "LightBar=")) ++logs; }
void sleep_us(std::uint32_t) {}
}

int main() {
    try {
        pad_light();
        std::cout << "PASS " << checks << " native Pad light checks; same handle, native set/reset, bounded errors.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}

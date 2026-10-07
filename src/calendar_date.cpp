// Local-calendar date with independent, persistent display components.
// Copyright (C) 2026 Stremio PS5 contributors
// SPDX-License-Identifier: GPL-3.0-or-later
#include "calendar_date.h"
#include <array>
#include <cstdio>
#include <vector>

namespace {
constexpr std::array<const char*, 7> days_it = {"domenica", "lunedì", "martedì", "mercoledì", "giovedì", "venerdì", "sabato"};
constexpr std::array<const char*, 7> days_en = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
constexpr std::array<const char*, 7> short_days_it = {"dom", "lun", "mar", "mer", "gio", "ven", "sab"};
constexpr std::array<const char*, 7> short_days_en = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr std::array<const char*, 12> months_it = {"gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio", "agosto", "settembre", "ottobre", "novembre", "dicembre"};
constexpr std::array<const char*, 12> months_en = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
constexpr std::array<const char*, 12> short_months_it = {"gen", "feb", "mar", "apr", "mag", "giu", "lug", "ago", "set", "ott", "nov", "dic"};
constexpr std::array<const char*, 12> short_months_en = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
std::string padded(int value) {
    char text[16]; std::snprintf(text, sizeof(text), "%02d", value); return text;
}
std::string combine(const std::vector<std::string>& parts, const char* separator) {
    std::string result;
    for (const auto& part : parts) {
        if (!result.empty()) result += separator;
        result += part;
    }
    return result;
}
}

std::string calendar_date(const std::tm& local, const std::string& language,
                          const std::string& format, int components) {
    components &= kAllDateComponents;
    if (!components || local.tm_wday < 0 || local.tm_wday > 6 ||
        local.tm_mon < 0 || local.tm_mon > 11 || local.tm_mday < 1 || local.tm_mday > 31) return {};
    const bool italian = language != "en", extended = format == "long";
    const bool numeric = format == "numeric", iso = format == "iso";
    std::string weekday;
    if (components & DateWeekday)
        weekday = extended ? (italian ? days_it : days_en)[local.tm_wday]
                           : (italian ? short_days_it : short_days_en)[local.tm_wday];
    std::vector<std::string> parts;
    const auto day = [&] { if (components & DateDay) parts.push_back(numeric || iso ? padded(local.tm_mday) : std::to_string(local.tm_mday)); };
    const auto month = [&] {
        if (!(components & DateMonth)) return;
        parts.push_back(numeric || iso ? padded(local.tm_mon + 1)
            : extended ? (italian ? months_it : months_en)[local.tm_mon]
                       : (italian ? short_months_it : short_months_en)[local.tm_mon]);
    };
    const auto year = [&] { if (components & DateYear) parts.push_back(std::to_string(local.tm_year + 1900)); };
    if (iso) { year(); month(); day(); }
    else if (italian) { day(); month(); year(); }
    else { month(); day(); year(); }
    std::string result = combine(parts, iso ? "-" : numeric ? "/" : " ");
    if (!weekday.empty()) result = weekday + (result.empty() ? "" : " ") + result;
    return result;
}

std::string calendar_date_now(const std::string& language, const std::string& format, int components) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    if (!localtime_r(&now, &local)) return {};
    return calendar_date(local, language, format, components);
}

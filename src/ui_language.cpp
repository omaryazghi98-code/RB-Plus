// Interface language selection with an English fallback.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_language.h"
#include "util.h"
#include <algorithm>
#include <cctype>
#ifdef PLATFORM_PS5_NATIVE
extern "C" int sceSystemServiceParamGetInt(int parameter, int* value);
#else
#include <SDL_locale.h>
#endif

std::string supported_ui_language(const std::string& locale) {
    const auto end = locale.find_first_of("-_.@");
    auto primary = locale.substr(0, end);
    std::transform(primary.begin(), primary.end(), primary.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return primary == "it" ? "it" : "en";
}
std::string resolve_ui_language(const std::string& preference, const std::string& system_locale) {
    if (preference == "it" || preference == "en") return preference;
    return supported_ui_language(system_locale);
}
std::string ps5_ui_language(int query_result, int system_language) {
    // Public system-service parameter mapping: language = 1, Italian = 5.
    // https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/master/include/orbis/_types/sys_service.h
    return query_result == 0 && system_language == 5 ? "it" : "en";
}
std::string platform_ui_language() {
#ifdef PLATFORM_PS5_NATIVE
    int language = -1;
    const int result = sceSystemServiceParamGetInt(1, &language);
    const auto selected = ps5_ui_language(result, language);
    dlog("Interface system language query: result=%d code=%d selected=%s", result, language, selected.c_str());
    return selected;
#else
    SDL_Locale* locales = SDL_GetPreferredLocales();
    const auto selected = supported_ui_language(locales && locales[0].language ? locales[0].language : "en");
    SDL_free(locales);
    return selected;
#endif
}

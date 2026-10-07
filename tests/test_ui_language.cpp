// Exercise the real native locale query with a controlled system-service boundary.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ui_language.h"
#include <iostream>
#include <stdexcept>

namespace {
int checks = 0, calls = 0, query_result = 0, system_language = 5, queried_parameter = -1;
void expect(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
}
void dlog(const char*, ...) {}
extern "C" int sceSystemServiceParamGetInt(int parameter, int* value) {
    ++calls; queried_parameter = parameter;
    if (query_result == 0) *value = system_language;
    return query_result;
}
int main() {
    try {
        for (const auto* locale : {"it", "it-IT", "it_IT.UTF-8", "IT", "it@euro"})
            expect(supported_ui_language(locale) == "it", "Italian locale variants normalize to the available translation");
        for (const auto* locale : {"en", "en-GB", "en_US.UTF-8", "fr-FR", "de", "ja", "", "C", "invalid"})
            expect(supported_ui_language(locale) == "en", "English or unsupported locale uses English");
        expect(resolve_ui_language("en", "it") == "en", "explicit English overrides system Italian");
        expect(resolve_ui_language("it", "en") == "it", "explicit Italian overrides system English");
        expect(resolve_ui_language("auto", "it-IT") == "it", "Automatic uses a supported system language");
        expect(resolve_ui_language("auto", "pt-BR") == "en", "Automatic falls back to English");
        expect(platform_ui_language() == "it" && calls == 1 && queried_parameter == 1,
            "native path invokes the language parameter once and maps Italian code5");
        for (const int language : {0, 1, 2, 3, 4, 6, 7, 18, 999, -1}) {
            system_language = language;
            expect(platform_ui_language() == "en", "other native language codes use English");
        }
        for (const int result : {-1, -2147352576, 1}) {
            query_result = result; system_language = 5;
            expect(platform_ui_language() == "en", "failed or unexpected native query returns English without reading an uninitialized value");
        }
        std::cout << "UI_LANGUAGE_TESTS_OK checks=" << checks << " native_boundary_calls=" << calls << '\n';
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "UI_LANGUAGE_TESTS_FAILED: " << e.what() << '\n';
        return 1;
    }
}

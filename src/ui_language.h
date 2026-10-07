// Interface language selection. Media track preferences are independent.
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

std::string supported_ui_language(const std::string& locale);
std::string resolve_ui_language(const std::string& preference, const std::string& system_locale);
std::string ps5_ui_language(int query_result, int system_language);
std::string platform_ui_language();

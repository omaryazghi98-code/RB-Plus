#pragma once

#include <ctime>
#include <string>

enum DateComponent { DateWeekday = 1, DateDay = 2, DateMonth = 4, DateYear = 8 };
constexpr int kAllDateComponents = DateWeekday | DateDay | DateMonth | DateYear;

std::string calendar_date(const std::tm& local, const std::string& language,
                          const std::string& format, int components);
std::string calendar_date_now(const std::string& language, const std::string& format, int components);

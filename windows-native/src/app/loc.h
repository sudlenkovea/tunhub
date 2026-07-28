#pragma once
// Interface strings. English is the source language; translations override by key.

#include <string>

namespace tunhub::app::loc {

/// "system" follows the OS language; otherwise "en" or "ru".
void setLanguage(const std::string& code);

/// Translated string (UTF-8). Unknown keys return the key itself, which is the English text.
const std::string& t(const std::string& key);

/// Translated string as UTF-16, ready for Win32.
std::wstring w(const std::string& key);

}  // namespace tunhub::app::loc

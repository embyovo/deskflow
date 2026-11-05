/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/FocusPause.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace {

//! Case insensitive ASCII compare, \p lower is already lower case.
bool equalsIgnoreCase(std::string_view value, std::string_view lower)
{
  if (value.size() != lower.size()) {
    return false;
  }
  for (size_t i = 0; i < value.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) != lower[i]) {
      return false;
    }
  }
  return true;
}

//! Case insensitive search, \p lower is already lower case.
bool containsIgnoreCase(std::string_view text, std::string_view lower)
{
  if (lower.size() > text.size()) {
    return false;
  }
  for (size_t start = 0; start + lower.size() <= text.size(); ++start) {
    bool match = true;
    for (size_t i = 0; i < lower.size() && match; ++i) {
      match = std::tolower(static_cast<unsigned char>(text[start + i])) == lower[i];
    }
    if (match) {
      return true;
    }
  }
  return false;
}

std::string_view baseName(std::string_view path)
{
  const auto separator = path.find_last_of("\\/");
  return separator == std::string_view::npos ? path : path.substr(separator + 1);
}

} // namespace

namespace deskflow::server {

FocusPause::FocusPause(std::set<std::string> processNames, std::vector<std::string> windowTitles)
{
  setRules(std::move(processNames), std::move(windowTitles));
}

void FocusPause::setRules(std::set<std::string> processNames, std::vector<std::string> windowTitles)
{
  m_processNames.clear();
  for (const auto &name : processNames) {
    if (const auto clean = processNameFromPath(trim(name)); !clean.empty()) {
      m_processNames.insert(clean);
    }
  }

  m_windowTitles.clear();
  for (const auto &title : windowTitles) {
    if (const auto clean = toLowerAscii(trim(title)); !clean.empty()) {
      m_windowTitles.push_back(clean);
    }
  }
}

bool FocusPause::isEmpty() const
{
  return m_processNames.empty();
}

bool FocusPause::shouldPause(const ActiveAppInfo &app) const
{
  // an empty list, or a platform that can not report the focused app, never
  // pauses switching.  the window title alone is not enough to identify an app.
  if (m_processNames.empty() || app.m_processName.empty()) {
    return false;
  }

  // only a full screen application pauses switching: a windowed one must not
  // trap the cursor on the computer it runs on
  if (!app.m_fullscreen) {
    return false;
  }

  const auto name = baseName(app.m_processName);
  const bool matches =
      std::ranges::any_of(m_processNames, [name](const auto &rule) { return equalsIgnoreCase(name, rule); });
  if (!matches) {
    return false;
  }

  // the title list is optional and narrows the match, e.g. only pause while
  // chrome shows a window titled "video".  this runs every 200ms, so it must
  // not copy the title to compare it.
  return m_windowTitles.empty() || std::ranges::any_of(m_windowTitles, [&app](const auto &fragment) {
    return containsIgnoreCase(app.m_windowTitle, fragment);
  });
}

std::string FocusPause::processNameFromPath(const std::string &path)
{
  return toLowerAscii(std::string(baseName(path)));
}

std::string FocusPause::toLowerAscii(const std::string &value)
{
  std::string result(value);
  std::ranges::transform(result, result.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

std::string FocusPause::trim(const std::string &value)
{
  const auto begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const auto end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

} // namespace deskflow::server

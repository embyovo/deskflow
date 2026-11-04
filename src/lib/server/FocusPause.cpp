/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "server/FocusPause.h"

#include <algorithm>
#include <cctype>

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
  if (isEmpty() || app.m_processName.empty()) {
    return false;
  }

  if (!matchesProcess(app.m_processName, m_processNames)) {
    return false;
  }

  // the title list is optional and narrows the match, e.g. only pause while
  // chrome shows a window titled "video"
  return m_windowTitles.empty() || matchesTitle(app.m_windowTitle, m_windowTitles);
}

std::string FocusPause::processNameFromPath(const std::string &path)
{
  const auto separator = path.find_last_of("\\/");
  const auto name = separator == std::string::npos ? path : path.substr(separator + 1);
  return toLowerAscii(name);
}

bool FocusPause::matchesProcess(const std::string &processName, const std::set<std::string> &processNames)
{
  return processNames.contains(processNameFromPath(processName));
}

bool FocusPause::matchesTitle(const std::string &windowTitle, const std::vector<std::string> &windowTitles)
{
  const auto title = toLowerAscii(windowTitle);
  return std::ranges::any_of(
      windowTitles, [&title](const auto &fragment) { return title.find(fragment) != std::string::npos; }
  );
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

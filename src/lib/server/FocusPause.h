/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/ActiveAppInfo.h"

#include <set>
#include <string>
#include <vector>

namespace deskflow::server {

//! Matches the focused application against the pause list
/*!
The pause list contains executable file names (e.g. "chrome.exe") and optional
window title fragments.  A process name is always required; when title fragments
are configured the window title must also contain one of them.  Matching is
case insensitive for ASCII characters and the list is never empty by default,
so the feature is off until the user configures it.
*/
class FocusPause
{
public:
  FocusPause() = default;
  FocusPause(std::set<std::string> processNames, std::vector<std::string> windowTitles);

  //! Replace the pause list
  void setRules(std::set<std::string> processNames, std::vector<std::string> windowTitles);

  //! Returns true when no process name is configured
  bool isEmpty() const;

  //! Returns true when switching should pause while \p app has focus
  bool shouldPause(const ActiveAppInfo &app) const;

  //! Reduce a path or name to a lower case executable file name
  static std::string processNameFromPath(const std::string &path);

private:
  static std::string toLowerAscii(const std::string &value);
  static std::string trim(const std::string &value);

private:
  std::set<std::string> m_processNames;
  std::vector<std::string> m_windowTitles;
};

} // namespace deskflow::server

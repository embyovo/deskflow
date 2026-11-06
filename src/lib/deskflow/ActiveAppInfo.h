/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <string>

//! Application that owns the window with keyboard focus
/*!
Used by the server to pause computer switching while the user works in
specific applications.  Both members are empty when the platform can not
report it or when no window has focus.
*/
struct ActiveAppInfo
{
  //! Executable file name, e.g. "chrome.exe", in the original case
  std::string m_processName;

  //! Title of the focused top level window, in the original case
  std::string m_windowTitle;

  //! True when the focused window covers its whole monitor
  /*!
  Only full screen applications pause switching, a windowed application must
  never trap the cursor on the computer it runs on.  Platforms that can not
  tell leave this false.
  */
  bool m_fullscreen = false;
};

/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QString>

namespace deskflow::gui {

//! One entry of the process choice list in the pause on focus settings
struct PauseAppChoice
{
  //! Executable name, the only value that is stored in the configuration
  QString m_executable;
  //! Friendly name shown to the user, may be empty
  QString m_description;
  //! 0 = foreground, 1 = has a visible window, 2 = everything else
  int m_group = 2;

  //! Text shown in the combo box, "description (executable)" when a description exists
  QString label() const;
};

//! Order of the process choice list: group first, then the displayed name
bool pauseAppLess(const PauseAppChoice &left, const PauseAppChoice &right);

} // namespace deskflow::gui

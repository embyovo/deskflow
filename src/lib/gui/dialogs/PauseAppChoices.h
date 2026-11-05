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
  //! 0 = foreground (visible, not minimized), 1 = minimized or hidden, 2 = no window
  int m_group = 2;
  //! True for the application that owns the foreground window, only used for the label and the order
  bool m_current = false;

  //! Text shown in the combo box, "description (executable)" when a description exists
  QString label() const;
};

//! Group of an application from the state of its windows, foreground wins over background
int pauseAppGroup(bool hasForegroundWindow, bool hasBackgroundWindow);

//! Order of the process choice list: group first, the current application next, then the displayed name
bool pauseAppLess(const PauseAppChoice &left, const PauseAppChoice &right);

//! The executable that should be marked as the foreground application.
/*!
 * \param current Executable of the real foreground window, empty when unknown
 * \param ownExecutable Our own executable, which owns the foreground while the dialog is open
 * \param recent Executable of the most recent window in z order behind our own
 * \return The application the user came from, empty when it cannot be told apart from us
 */
QString foregroundExecutable(const QString &current, const QString &ownExecutable, const QString &recent);

} // namespace deskflow::gui

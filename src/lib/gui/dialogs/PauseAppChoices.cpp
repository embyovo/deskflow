/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "PauseAppChoices.h"

namespace deskflow::gui {

QString PauseAppChoice::label() const
{
  if (m_description.isEmpty())
    return m_executable;
  return QStringLiteral("%1 (%2)").arg(m_description, m_executable);
}

bool pauseAppLess(const PauseAppChoice &left, const PauseAppChoice &right)
{
  if (left.m_group != right.m_group)
    return left.m_group < right.m_group;
  return left.label().compare(right.label(), Qt::CaseInsensitive) < 0;
}

QString foregroundExecutable(const QString &current, const QString &ownExecutable, const QString &recent)
{
  if (!current.isEmpty() && current.compare(ownExecutable, Qt::CaseInsensitive) != 0)
    return current;
  if (!recent.isEmpty() && recent.compare(ownExecutable, Qt::CaseInsensitive) != 0)
    return recent;
  return {};
}

} // namespace deskflow::gui

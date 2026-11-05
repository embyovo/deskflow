/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class FocusPauseTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void emptyListNeverPauses();
  void matchesProcessName();
  void processNameFromPath();
  void ignoresBlankRules();
  void titleNarrowsMatch();
  void titleWithoutProcessNeverPauses();
  void unknownActiveAppNeverPauses();
  void windowedAppNeverPauses();
  void staysPausedWhenSwitchingBetweenListedApps();
  void resumesAfterFocusLeaves();
};

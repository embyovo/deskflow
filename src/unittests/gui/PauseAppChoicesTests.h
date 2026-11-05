/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QTest>

class PauseAppChoicesTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void groupsAreOrderedForegroundBackgroundOther();
  void sortsByDisplayNameInsideGroup();
  void descriptionIsUsedForDisplayAndOrder();
  void missingDescriptionFallsBackToExecutable();
  void foregroundIsKeptWhenItIsAnotherApplication();
  void ownWindowFallsBackToTheRecentApplication();
  void ownWindowWithoutRecentHasNoForeground();
  void emptyForegroundFallsBackToTheRecentApplication();
  void visibleWindowIsForegroundGroup();
  void minimizedWindowIsBackgroundGroup();
  void processWithoutWindowIsOtherGroup();
  void currentForegroundIsSortedFirstInsideGroup();
};

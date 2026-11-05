/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "PauseAppChoicesTests.h"

#include "gui/dialogs/PauseAppChoices.h"

using namespace deskflow::gui;

void PauseAppChoicesTests::groupsAreOrderedForegroundWindowedOther()
{
  PauseAppChoice foreground{QStringLiteral("a.exe"), {}, 0};
  PauseAppChoice windowed{QStringLiteral("b.exe"), {}, 1};
  PauseAppChoice other{QStringLiteral("c.exe"), {}, 2};

  // the task manager order: foreground application, windowed applications, the rest
  QVERIFY(pauseAppLess(foreground, windowed));
  QVERIFY(pauseAppLess(windowed, other));
  QVERIFY(pauseAppLess(foreground, other));
  QVERIFY(!pauseAppLess(other, windowed));
  QVERIFY(!pauseAppLess(foreground, foreground));

  // a foreground application with a late name still comes before an early background one
  QCOMPARE(pauseAppLess(PauseAppChoice{QStringLiteral("zeta.exe"), {}, 0}, windowed), true);
}

void PauseAppChoicesTests::sortsByDisplayNameInsideGroup()
{
  PauseAppChoice alpha{QStringLiteral("alpha.exe"), {}, 1};
  PauseAppChoice beta{QStringLiteral("beta.exe"), {}, 1};

  // case is ignored, the task manager sorts names without caring about case
  QVERIFY(!pauseAppLess(PauseAppChoice{QStringLiteral("Beta.exe"), {}, 1}, alpha));
  QVERIFY(pauseAppLess(alpha, PauseAppChoice{QStringLiteral("Beta.exe"), {}, 1}));
  QVERIFY(pauseAppLess(alpha, beta));
  QVERIFY(!pauseAppLess(beta, alpha));
}

void PauseAppChoicesTests::descriptionIsUsedForDisplayAndOrder()
{
  PauseAppChoice zebra{QStringLiteral("aaa.exe"), QStringLiteral("Zebra Tool"), 1};
  PauseAppChoice alpha{QStringLiteral("zzz.exe"), QStringLiteral("Alpha Tool"), 1};

  QCOMPARE(zebra.label(), QStringLiteral("Zebra Tool (aaa.exe)"));
  // the executable names would sort the other way round, the friendly name decides
  QVERIFY(!pauseAppLess(zebra, alpha));
  QVERIFY(pauseAppLess(alpha, zebra));
}

void PauseAppChoicesTests::missingDescriptionFallsBackToExecutable()
{
  PauseAppChoice unknown{QStringLiteral("custom.exe"), QString(), 2};
  QCOMPARE(unknown.label(), QStringLiteral("custom.exe"));

  PauseAppChoice described{QStringLiteral("custom.exe"), QStringLiteral("Custom Tool"), 2};
  QCOMPARE(described.label(), QStringLiteral("Custom Tool (custom.exe)"));
}

void PauseAppChoicesTests::foregroundIsKeptWhenItIsAnotherApplication()
{
  QCOMPARE(
      foregroundExecutable(QStringLiteral("editor.exe"), QStringLiteral("deskflow.exe"), QStringLiteral("browser.exe")),
      QStringLiteral("editor.exe")
  );
}

void PauseAppChoicesTests::ownWindowFallsBackToTheRecentApplication()
{
  // while the settings dialog is open GetForegroundWindow returns our own window
  QCOMPARE(
      foregroundExecutable(
          QStringLiteral("Deskflow.exe"), QStringLiteral("deskflow.exe"), QStringLiteral("msedge.exe")
      ),
      QStringLiteral("msedge.exe")
  );
}

void PauseAppChoicesTests::ownWindowWithoutRecentHasNoForeground()
{
  QVERIFY(foregroundExecutable(QStringLiteral("deskflow.exe"), QStringLiteral("deskflow.exe"), QString()).isEmpty());
  // a recent window of our own process is not an application either
  QVERIFY(foregroundExecutable(QString(), QStringLiteral("deskflow.exe"), QStringLiteral("Deskflow.exe")).isEmpty());
}

void PauseAppChoicesTests::emptyForegroundFallsBackToTheRecentApplication()
{
  QCOMPARE(
      foregroundExecutable(QString(), QStringLiteral("deskflow.exe"), QStringLiteral("browser.exe")),
      QStringLiteral("browser.exe")
  );
}

QTEST_APPLESS_MAIN(PauseAppChoicesTests)

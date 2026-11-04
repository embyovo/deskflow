/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "FocusPauseTests.h"

#include "server/FocusPause.h"

using namespace deskflow::server;

void FocusPauseTests::emptyListNeverPauses()
{
  FocusPause pause;
  QVERIFY(pause.isEmpty());
  QVERIFY(!pause.shouldPause({"chrome.exe", "Google Chrome"}));
}

void FocusPauseTests::matchesProcessName()
{
  FocusPause pause({"chrome.exe"}, {});
  QVERIFY(!pause.isEmpty());

  // process names are compared without case
  QVERIFY(pause.shouldPause({"Chrome.exe", "Google Chrome"}));
  QVERIFY(!pause.shouldPause({"notepad.exe", "Untitled - Notepad"}));
}

void FocusPauseTests::processNameFromPath()
{
  QCOMPARE(FocusPause::processNameFromPath("C:\\Program Files\\Google\\Chrome\\chrome.exe"), "chrome.exe");
  QCOMPARE(FocusPause::processNameFromPath("/usr/bin/firefox"), "firefox");
  QCOMPARE(FocusPause::processNameFromPath("CHROME.EXE"), "chrome.exe");
}

void FocusPauseTests::ignoresBlankRules()
{
  FocusPause pause({"", "  ", "notepad.exe"}, {"", "  "});
  QVERIFY(!pause.isEmpty());
  QVERIFY(!pause.shouldPause({"chrome.exe", "Google Chrome"}));
  QVERIFY(pause.shouldPause({"notepad.exe", "Untitled - Notepad"}));
}

void FocusPauseTests::titleNarrowsMatch()
{
  FocusPause pause({"chrome.exe"}, {"youtube"});
  QVERIFY(pause.shouldPause({"chrome.exe", "YouTube - Google Chrome"}));
  QVERIFY(!pause.shouldPause({"chrome.exe", "Deskflow - Google Chrome"}));

  // any configured title fragment is enough
  FocusPause several({"chrome.exe"}, {"youtube", "netflix"});
  QVERIFY(several.shouldPause({"chrome.exe", "Netflix - Google Chrome"}));
}

void FocusPauseTests::titleWithoutProcessNeverPauses()
{
  FocusPause pause({}, {"youtube"});
  QVERIFY(pause.isEmpty());
  QVERIFY(!pause.shouldPause({"chrome.exe", "YouTube - Google Chrome"}));
}

void FocusPauseTests::unknownActiveAppNeverPauses()
{
  // e.g. the desktop has focus or the platform does not report the app
  FocusPause pause({"chrome.exe"}, {});
  QVERIFY(!pause.shouldPause({}));
  QVERIFY(!pause.shouldPause({"", ""}));
}

void FocusPauseTests::staysPausedWhenSwitchingBetweenListedApps()
{
  FocusPause pause({"chrome.exe", "notepad.exe"}, {});
  QVERIFY(pause.shouldPause({"chrome.exe", "Google Chrome"}));
  QVERIFY(pause.shouldPause({"notepad.exe", "Untitled - Notepad"}));
}

void FocusPauseTests::resumesAfterFocusLeaves()
{
  FocusPause pause({"notepad.exe"}, {});
  QVERIFY(pause.shouldPause({"notepad.exe", "Untitled - Notepad"}));

  // focus returns to the desktop (explorer) or any other app: pause is over
  QVERIFY(!pause.shouldPause({"explorer.exe", "Program Manager"}));
  QVERIFY(!pause.shouldPause({"chrome.exe", "Google Chrome"}));

  // and comes back as soon as the listed app is focused again
  QVERIFY(pause.shouldPause({"notepad.exe", "Untitled - Notepad"}));
}

QTEST_MAIN(FocusPauseTests)

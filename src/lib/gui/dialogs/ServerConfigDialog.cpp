/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 - 2026 Chris Rizzitello <sithlord48@gmail.com>
 * SPDX-FileCopyrightText: (C) 2012 - 2016 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2008 Volker Lanz <vl@fidra.de>
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerConfigDialog.h"
#include "ui_ServerConfigDialog.h"

#include "common/Constants.h"
#include "common/NetworkProtocol.h"
#include "common/PlatformInfo.h"
#include "common/Settings.h"
#include "dialogs/ActionDialog.h"
#include "dialogs/ComputerSettingsDialog.h"
#include "dialogs/HotkeyDialog.h"
#include "gui/widgets/SettingsDialogButtonBox.h"

#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QListWidgetItem>
#include <QMessageBox>

#include <algorithm>

#ifdef Q_OS_WIN
// clang-format off
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <TlHelp32.h>
#include <winver.h>
// clang-format on
#include <cwchar>
#include <vector>
#endif

using enum ComputerConfig::SwitchCorner;

namespace {
#ifdef Q_OS_WIN
struct RunningProcess
{
  QString m_name;
  QString m_description;
};

//! Full path of a process, or an empty string when it cannot be queried (protected or gone).
QString processImagePath(DWORD processId)
{
  QString path;
  if (const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId); process != nullptr) {
    wchar_t buffer[1024];
    DWORD length = 1024;
    if (QueryFullProcessImageNameW(process, 0, buffer, &length) && length > 0)
      path = QString::fromWCharArray(buffer, static_cast<int>(length));
    CloseHandle(process);
  }
  return path;
}

QString fileNameFromPath(const QString &path)
{
  const int separator = qMax(path.lastIndexOf(QLatin1Char('\\')), path.lastIndexOf(QLatin1Char('/')));
  return path.mid(separator + 1);
}

//! The FileDescription shown by Task Manager, for example "Google Chrome".
QString fileDescription(const QString &path)
{
  if (path.isEmpty())
    return {};

  DWORD ignored = 0;
  const auto size = GetFileVersionInfoSizeW(reinterpret_cast<LPCWSTR>(path.utf16()), &ignored);
  if (size == 0)
    return {};

  std::vector<BYTE> versionInfo(size);
  if (!GetFileVersionInfoW(reinterpret_cast<LPCWSTR>(path.utf16()), 0, size, versionInfo.data()))
    return {};

  struct Translation
  {
    WORD m_language;
    WORD m_codePage;
  };
  LPVOID translations = nullptr;
  UINT translationLength = 0;
  if (!VerQueryValueW(versionInfo.data(), L"\\VarFileInfo\\Translation", &translations, &translationLength) ||
      translationLength < sizeof(Translation))
    return {};

  const auto *translation = static_cast<const Translation *>(translations);
  wchar_t key[64];
  std::swprintf(
      key, std::size(key), L"\\StringFileInfo\\%04x%04x\\FileDescription", translation->m_language,
      translation->m_codePage
  );
  wchar_t *text = nullptr;
  UINT textLength = 0;
  if (!VerQueryValueW(versionInfo.data(), key, reinterpret_cast<void **>(&text), &textLength) || textLength == 0)
    return {};

  return QString::fromWCharArray(text).trimmed();
}

QList<RunningProcess> runningProcesses()
{
  QList<RunningProcess> processes;
  const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE)
    return processes;

  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      RunningProcess process;
      process.m_name = QString::fromWCharArray(entry.szExeFile);
      if (process.m_name.isEmpty())
        continue;
      process.m_description = fileDescription(processImagePath(entry.th32ProcessID));
      processes.append(process);
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return processes;
}

//! The executable that owns the foreground window.
/*!
 * The GUI runs in its own process and cannot reach the computer object of the
 * server, so the platform hook used by the server is not available here. Keep
 * this Windows only; other platforms simply offer the manual input.
 */
QString foregroundProcessName()
{
  const HWND window = GetForegroundWindow();
  if (window == nullptr)
    return {};

  DWORD processId = 0;
  if (GetWindowThreadProcessId(window, &processId) == 0 || processId == 0)
    return {};

  return fileNameFromPath(processImagePath(processId));
}
#else
struct RunningProcess
{
  QString m_name;
  QString m_description;
};

QList<RunningProcess> runningProcesses()
{
  return {};
}

QString foregroundProcessName()
{
  return {};
}
#endif
} // namespace


ServerConfigDialog::ServerConfigDialog(QWidget *parent, ServerConfig &config)
    : QDialog(parent, Qt::WindowTitleHint | Qt::WindowSystemMenuHint),
      ui{std::make_unique<Ui::ServerConfigDialog>()},
      m_columns{Settings::value(Settings::Server::GridWidth).toInt()},
      m_rows{Settings::value(Settings::Server::GridHeight).toInt()},
      m_originalServerConfig(config),
      m_originalServerConfigIsExternal(config.useExternalConfig()),
      m_originalServerConfigUsesExternalFile(config.configFile()),
      m_serverConfig(config),
      m_computerSetupModel(m_serverConfig.computers(), m_columns, m_rows),
      m_buttonBox{new SettingsDialogButtonBox(this)}
{
  ui->setupUi(this);
  ui->tabWidget->setCurrentIndex(0);
  layout()->addWidget(m_buttonBox);

  loadFromConfig();

  ui->lblRemoveComputer->setPixmap(QIcon::fromTheme("user-trash").pixmap(QSize(64, 64)));
  ui->lblNewComputer->setEnabled(!model().isFull());
  ui->lblNewComputer->setPixmap(QIcon::fromTheme("video-display").pixmap(QSize(64, 64)));
  ui->btnBrowseConfigFile->setIcon(QIcon::fromTheme(QIcon::ThemeIcon::DocumentOpen));

  if (!deskflow::platform::isWindows())
    ui->cbWin32KeepForeground->setVisible(false);
  ui->cbPauseApp->setEditable(true);
  ui->cbPauseApp->setInsertPolicy(QComboBox::NoInsert);
  ui->cbPauseApp->setSizeAdjustPolicy(QComboBox::AdjustToContents);
  populatePauseAppChoices();
  initConnections();
}

ServerConfigDialog::~ServerConfigDialog() = default;

bool ServerConfigDialog::addClient(const QString &clientName)
{
  return addComputer(clientName, true);
}

void ServerConfigDialog::save()
{
  if (ui->groupExternalConfig->isChecked() && !QFile::exists(ui->lineConfigFile->text())) {

    auto selectedButton = QMessageBox::warning(
        this, "Filename invalid", "Please select a valid configuration file.", QMessageBox::Ok | QMessageBox::Ignore
    );

    if (selectedButton != QMessageBox::Ok || !browseConfigFile()) {
      return;
    }
  }
  // now that the dialog has been accepted, copy the new server config to the
  // original one, which is a reference to the one in MainWindow.
  setOriginalServerConfig(serverConfig());
  if (Settings::value(Settings::Server::EnableClipboard).toBool() != m_enableClipboard) {
    Settings::setValue(Settings::Server::XdpRestoreToken, QString());
    Settings::setValue(Settings::Server::XdpClipboardRetried, false);
  }
  Settings::setValue(Settings::Server::Protocol, networkProtocolToOption(m_protocol));
  Settings::setValue(Settings::Server::EnableClipboard, m_enableClipboard);
  Settings::setValue(Settings::Server::ClipboardSize, m_clipboardSize);
  Settings::setValue(Settings::Server::EnableHeartbeat, m_enableHeartbeat);
  Settings::setValue(Settings::Server::Heartbeat, m_heartbeatRate);
  Settings::setValue(Settings::Server::EnableSwitchDelay, m_enableSwitchDelay);
  Settings::setValue(Settings::Server::SwitchDelay, m_switchDelay);
  Settings::setValue(Settings::Server::DefaultLockToComputerState, m_defaultLockToComputerState);
  Settings::setValue(Settings::Server::DisableLockToComputer, m_disableLockToComputer);
  Settings::setValue(Settings::Server::EnableSwitchDoubleTap, m_enableSwitchDoubleTap);
  Settings::setValue(Settings::Server::SwitchDoubleTap, m_switchDoubleTap);
  Settings::setValue(Settings::Server::RelativeMouseMoves, m_relativeMouseMoves);
  Settings::setValue(Settings::Server::Win32KeepForeground, m_win32keepForeground);
  Settings::setValue(Settings::Server::ExternalConfig, ui->groupExternalConfig->isChecked());
  Settings::setValue(Settings::Server::ExternalConfigFile, ui->lineConfigFile->text());
  Settings::setValue(Settings::Server::PauseOnFocusApps, m_pauseApps);
  Settings::setValue(Settings::Server::PauseOnFocusTitles, pauseTitles());

  QStringList computerNames;
  const auto computerList = m_computerSetupModel.m_computers;
  for (const auto &computer : computerList) {
    const auto &computerName = computer.name();
    if (computerName.isEmpty())
      continue;
    computerNames.append(QStringLiteral("computer_%1").arg(computerName));
    Settings::setValue(Settings::Computer::Aliases.arg(computerName), computer.aliases());
    Settings::setValue(Settings::Computer::Name.arg(computerName), computerName);
  }
  // Write the computer list now too, or it keeps computers whose settings were just removed until the app quits.
  m_serverConfig.commit();
  Settings::removeUnknownComputers(computerNames);
  QDialog::accept();
}

void ServerConfigDialog::cancel()
{
  serverConfig().setUseExternalConfig(m_originalServerConfigIsExternal);
  serverConfig().setConfigFile(m_originalServerConfigUsesExternalFile);
  QDialog::reject();
}

void ServerConfigDialog::addHotkey()
{
  Hotkey hotkey;
  HotkeyDialog dlg(this, hotkey);
  if (dlg.exec() == QDialog::Accepted) {
    serverConfig().hotkeys().append(hotkey);
    ui->listHotkeys->addItem(hotkey.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::editHotkey()
{
  int row = ui->listHotkeys->currentRow();
  if (row < 0 || row >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to editing out of bounds hotkey row: " << row;
    return;
  }

  Hotkey &hotkey = serverConfig().hotkeys()[row];
  HotkeyDialog dlg(this, hotkey);
  if (dlg.exec() == QDialog::Accepted) {
    ui->listHotkeys->currentItem()->setText(hotkey.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::removeHotkey()
{
  int row = ui->listHotkeys->currentRow();
  if (row < 0 || row >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to remove out of bounds hotkey row: " << row;
    return;
  }

  serverConfig().hotkeys().removeAt(row);
  ui->listActions->clear();
  delete ui->listHotkeys->item(row);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::listHotkeysSelectionChanged(const QItemSelection &selected, const QItemSelection &)
{
  bool itemsSelected = !selected.isEmpty();
  ui->btnEditHotkey->setEnabled(itemsSelected);
  ui->btnRemoveHotkey->setEnabled(itemsSelected);
  ui->btnNewAction->setEnabled(itemsSelected);

  if (itemsSelected && !serverConfig().hotkeys().isEmpty()) {
    ui->listActions->clear();
    const Hotkey &hotkey = serverConfig().hotkeys().at(selected.indexes().first().row());
    for (const Action &action : hotkey.actions())
      ui->listActions->addItem(action.text());
  }
}

void ServerConfigDialog::addAction()
{
  int row = ui->listHotkeys->currentRow();
  if (row < 0 || row >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to add action to out of bounds hotkey row: " << row;
    return;
  }

  Hotkey &hotkey = serverConfig().hotkeys()[row];
  Action action;
  ActionDialog dlg(this, serverConfig(), hotkey, action);
  if (dlg.exec() == QDialog::Accepted) {
    hotkey.addAction(action);
    ui->listActions->addItem(action.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::editAction()
{
  int hotkeyRow = ui->listHotkeys->currentRow();
  if (hotkeyRow < 0 || hotkeyRow >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to edit action from out of bounds hotkey row: " << hotkeyRow;
    return;
  }
  Hotkey &hotkey = serverConfig().hotkeys()[hotkeyRow];

  int actionRow = ui->listActions->currentRow();
  if (actionRow < 0 || actionRow >= hotkey.actions().size()) {
    qDebug() << "Attempt to remove out of bounds action row: " << actionRow;
    return;
  }
  Action &action = hotkey.actionAt(actionRow);

  ActionDialog dlg(this, serverConfig(), hotkey, action);
  if (dlg.exec() == QDialog::Accepted) {
    ui->listActions->currentItem()->setText(action.text());
    setButtonBoxEnabledButtons();
  }
}

void ServerConfigDialog::removeAction()
{
  int hotkeyRow = ui->listHotkeys->currentRow();
  if (hotkeyRow < 0 || hotkeyRow >= serverConfig().hotkeys().size()) {
    qDebug() << "Attempt to remove action from out of bounds hotkey row: " << hotkeyRow;
    return;
  }
  Hotkey &hotkey = serverConfig().hotkeys()[hotkeyRow];

  int actionRow = ui->listActions->currentRow();
  if (actionRow < 0 || actionRow >= hotkey.actions().size()) {
    qDebug() << "Attempt to remove out of bounds action row: " << actionRow;
    return;
  }

  hotkey.removeActionAt(actionRow);
  delete ui->listActions->currentItem();
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleClipboard(bool enabled)
{
  if (m_enableClipboard == enabled)
    return;

  m_enableClipboard = enabled;

  ui->sbClipboardSizeLimit->setEnabled(enabled);
  if (enabled && !ui->sbClipboardSizeLimit->value()) {
    m_clipboardSize = Settings::defaultValue(Settings::Server::ClipboardSize).toUInt();
    ui->sbClipboardSizeLimit->setValue(m_clipboardSize ? m_clipboardSize : 1);
  }
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setClipboardLimit(int limit)
{
  if (m_clipboardSize == limit)
    return;

  m_clipboardSize = limit;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleHeartbeat(bool enabled)
{
  m_enableHeartbeat = enabled;
  ui->sbHeartbeat->setEnabled(enabled);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setHeartbeat(int rate)
{
  if (rate == m_heartbeatRate)
    return;
  m_heartbeatRate = rate;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleRelativeMouseMoves(bool enabled)
{
  if (m_relativeMouseMoves == enabled)
    return;
  m_relativeMouseMoves = enabled;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleProtocol()
{
  m_protocol = ui->rbProtocolBarrier->isChecked() ? NetworkProtocol::Barrier : NetworkProtocol::Synergy;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::listActionsSelectionChanged(const QItemSelection &selected, const QItemSelection &)
{
  bool enabled = !selected.isEmpty();
  ui->btnEditAction->setEnabled(enabled);
  ui->btnRemoveAction->setEnabled(enabled);
}

void ServerConfigDialog::toggleSwitchDoubleTap(bool enable)
{
  m_enableSwitchDoubleTap = enable;
  ui->sbSwitchDoubleTap->setEnabled(enable);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setSwitchDoubleTap(int within)
{
  if (m_switchDoubleTap == within)
    return;
  m_switchDoubleTap = within;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleSwitchDelay(bool enable)
{
  m_enableSwitchDelay = enable;
  ui->sbSwitchDelay->setEnabled(enable);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::setSwitchDelay(int delay)
{
  if (m_switchDelay == delay)
    return;
  m_switchDelay = delay;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleDefaultLockToComputerState(bool state)
{
  if (m_defaultLockToComputerState == state)
    return;
  m_defaultLockToComputerState = state;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleLockToComputer(bool disabled)
{
  if (m_disableLockToComputer == disabled)
    return;
  m_disableLockToComputer = disabled;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleWin32Foreground(bool enabled)
{
  if (m_win32keepForeground == enabled)
    return;
  m_win32keepForeground = enabled;
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::addClient()
{
  addComputer("", false);
}

void ServerConfigDialog::onComputerRemoved()
{
  ui->lblNewComputer->setEnabled(true);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::toggleExternalConfig(bool checked)
{
  ui->widgetExternalConfigControls->setEnabled(checked);
  ui->tabWidget->setTabEnabled(0, !checked);
  ui->tabWidget->setTabEnabled(1, !checked);
  serverConfig().setUseExternalConfig(checked);
  setButtonBoxEnabledButtons();
}

bool ServerConfigDialog::browseConfigFile()
{
  //: %1 is replaced with the application names
  //: (*.conf) and (*.*) should not be translated
  const auto deskflowConfigFilter = tr("%1 Configurations (*.conf);;All files (*.*)");

  QString fileName =
      QFileDialog::getOpenFileName(this, tr("Browse for a config file"), "", deskflowConfigFilter.arg(kAppName));

  if (!fileName.isEmpty()) {
    ui->lineConfigFile->setText(fileName);
    setServerConfig();
    return true;
  }

  return false;
}

void ServerConfigDialog::loadFromConfig()
{
  m_protocol = Settings::networkProtocol();
  m_enableHeartbeat = Settings::value(Settings::Server::EnableHeartbeat).toBool();
  m_heartbeatRate = Settings::value(Settings::Server::Heartbeat).toInt();
  m_relativeMouseMoves = Settings::value(Settings::Server::RelativeMouseMoves).toBool();
  m_win32keepForeground = Settings::value(Settings::Server::Win32KeepForeground).toBool();
  m_enableSwitchDelay = Settings::value(Settings::Server::EnableSwitchDelay).toBool();
  m_switchDelay = Settings::value(Settings::Server::SwitchDelay).toInt();
  m_enableSwitchDoubleTap = Settings::value(Settings::Server::EnableSwitchDoubleTap).toBool();
  m_switchDoubleTap = Settings::value(Settings::Server::SwitchDoubleTap).toInt();
  m_defaultLockToComputerState = Settings::value(Settings::Server::DefaultLockToComputerState).toBool();
  m_disableLockToComputer = Settings::value(Settings::Server::DisableLockToComputer).toBool();
  m_enableClipboard = Settings::value(Settings::Server::EnableClipboard).toBool();
  m_clipboardSize = Settings::value(Settings::Server::ClipboardSize).toUInt();
  m_pauseApps = Settings::value(Settings::Server::PauseOnFocusApps).toStringList();
  ui->linePauseTitles->setText(Settings::value(Settings::Server::PauseOnFocusTitles).toStringList().join(", "));

  ui->lineConfigFile->setText(serverConfig().configFile());
  ui->groupExternalConfig->setChecked(serverConfig().useExternalConfig());

  refreshControls();

  ui->listHotkeys->clear();
  for (const Hotkey &hotkey : std::as_const(serverConfig().hotkeys()))
    ui->listHotkeys->addItem(hotkey.text());

  ui->computerSetupView->setModel(&m_computerSetupModel);

  auto &computers = serverConfig().computers();
  auto server = std::ranges::find_if(computers, [this](const Computer &computer) {
    return (computer.name() == serverConfig().getServerName());
  });

  if (server == computers.end()) {
    Computer serverComputer(serverConfig().getServerName());
    serverComputer.markAsServer();
    model().computer(m_columns / 2, m_rows / 2) = serverComputer;
  } else {
    server->markAsServer();
  }
  updateControls();
}

void ServerConfigDialog::resetFromSettings()
{
  m_serverConfig = m_originalServerConfig;
  m_serverConfig.setConfigFile(m_originalServerConfigUsesExternalFile);
  m_serverConfig.setUseExternalConfig(m_originalServerConfigIsExternal);
  loadFromConfig();
  if (ui->tabWidget->currentWidget() == ui->tabComputers) {
    ui->computerSetupView->reset();
    ui->computerSetupView->update();
  }
}

void ServerConfigDialog::refreshControls()
{
  ui->rbProtocolSynergy->setChecked(m_protocol == NetworkProtocol::Synergy);
  ui->rbProtocolBarrier->setChecked(m_protocol == NetworkProtocol::Barrier);
  ui->cbHeartbeat->setChecked(m_enableHeartbeat);
  ui->sbHeartbeat->setEnabled(ui->cbHeartbeat->isChecked());
  ui->sbHeartbeat->setValue(m_heartbeatRate);
  ui->cbRelativeMouseMoves->setChecked(m_relativeMouseMoves);
  ui->cbWin32KeepForeground->setChecked(m_win32keepForeground);
  ui->cbSwitchDelay->setChecked(m_enableSwitchDelay);
  ui->sbSwitchDelay->setEnabled(ui->cbSwitchDelay->isChecked());
  ui->sbSwitchDelay->setValue(m_switchDelay);
  ui->cbSwitchDoubleTap->setChecked(m_enableSwitchDoubleTap);
  ui->sbSwitchDoubleTap->setEnabled(ui->cbSwitchDoubleTap->isChecked());
  ui->sbSwitchDoubleTap->setValue(m_switchDoubleTap);
  ui->widgetExternalConfigControls->setEnabled(ui->groupExternalConfig->isChecked());
  toggleExternalConfig(ui->groupExternalConfig->isChecked());
  ui->cbDefaultLockToComputerState->setChecked(m_defaultLockToComputerState);
  ui->cbDisableLockToComputer->setChecked(m_disableLockToComputer);
  ui->cbEnableClipboard->setChecked(m_enableClipboard);
  ui->sbClipboardSizeLimit->setEnabled(m_enableClipboard);
  ui->sbClipboardSizeLimit->setValue(m_clipboardSize);
  refreshPauseAppList();
}

void ServerConfigDialog::initConnections() const
{
  connect(m_buttonBox, &SettingsDialogButtonBox::accepted, this, &ServerConfigDialog::save);
  connect(m_buttonBox, &SettingsDialogButtonBox::rejected, this, &ServerConfigDialog::cancel);
  connect(m_buttonBox, &SettingsDialogButtonBox::reset, this, &ServerConfigDialog::resetFromSettings);
  connect(m_buttonBox, &SettingsDialogButtonBox::restoreDefault, this, &ServerConfigDialog::restoreFromDefaults);
  connect(ui->tabWidget, &QTabWidget::currentChanged, this, &ServerConfigDialog::setButtonBoxEnabledButtons);
  connect(ui->lblRemoveComputer, &RemoveComputerWidget::computerRemoved, this, &ServerConfigDialog::onComputerRemoved);
  connect(ui->btnNewHotkey, &QPushButton::clicked, this, &ServerConfigDialog::addHotkey);
  connect(ui->btnEditHotkey, &QPushButton::clicked, this, &ServerConfigDialog::editHotkey);
  connect(ui->btnRemoveHotkey, &QPushButton::clicked, this, &ServerConfigDialog::removeHotkey);
  connect(ui->listHotkeys, &QListView::doubleClicked, this, &ServerConfigDialog::editHotkey);
  connect(
      ui->listHotkeys->selectionModel(), &QItemSelectionModel::selectionChanged, this,
      &ServerConfigDialog::listHotkeysSelectionChanged
  );

  connect(ui->btnNewAction, &QPushButton::clicked, this, &ServerConfigDialog::addAction);
  connect(ui->btnEditAction, &QPushButton::clicked, this, &ServerConfigDialog::editAction);
  connect(ui->btnRemoveAction, &QPushButton::clicked, this, &ServerConfigDialog::removeAction);
  connect(ui->listActions, &QListView::doubleClicked, this, &ServerConfigDialog::editAction);
  connect(
      ui->listActions->selectionModel(), &QItemSelectionModel::selectionChanged, this,
      &ServerConfigDialog::listActionsSelectionChanged
  );

  connect(ui->rbProtocolBarrier, &QRadioButton::toggled, this, &ServerConfigDialog::toggleProtocol);
  connect(ui->cbHeartbeat, &QCheckBox::toggled, this, &ServerConfigDialog::toggleHeartbeat);
  connect(ui->sbHeartbeat, QOverload<int>::of(&QSpinBox::valueChanged), this, &ServerConfigDialog::setHeartbeat);
  connect(ui->cbWin32KeepForeground, &QCheckBox::toggled, this, &ServerConfigDialog::toggleWin32Foreground);
  connect(ui->cbSwitchDelay, &QCheckBox::toggled, this, &ServerConfigDialog::toggleSwitchDelay);
  connect(ui->sbSwitchDelay, QOverload<int>::of(&QSpinBox::valueChanged), this, &ServerConfigDialog::setSwitchDelay);
  connect(ui->cbSwitchDoubleTap, &QCheckBox::toggled, this, &ServerConfigDialog::toggleSwitchDoubleTap);
  connect(
      ui->sbSwitchDoubleTap, QOverload<int>::of(&QSpinBox::valueChanged), this, &ServerConfigDialog::setSwitchDoubleTap
  );

  connect(ui->cbRelativeMouseMoves, &QCheckBox::toggled, this, &ServerConfigDialog::toggleRelativeMouseMoves);
  connect(ui->cbEnableClipboard, &QCheckBox::toggled, this, &ServerConfigDialog::toggleClipboard);
  connect(ui->btnBrowseConfigFile, &QPushButton::clicked, this, &ServerConfigDialog::browseConfigFile);
  connect(ui->groupExternalConfig, &QGroupBox::toggled, this, &ServerConfigDialog::toggleExternalConfig);
  connect(ui->lineConfigFile, &QLineEdit::textChanged, this, &ServerConfigDialog::setServerConfig);

  connect(
      ui->sbClipboardSizeLimit, QOverload<int>::of(&QSpinBox::valueChanged), this,
      &ServerConfigDialog::setClipboardLimit
  );
  connect(
      ui->cbDefaultLockToComputerState, &QCheckBox::toggled, this, &ServerConfigDialog::toggleDefaultLockToComputerState
  );
  connect(ui->cbDisableLockToComputer, &QCheckBox::toggled, this, &ServerConfigDialog::toggleLockToComputer);
  connect(
      &m_computerSetupModel, &ComputerSetupModel::computersChanged, this,
      &ServerConfigDialog::setButtonBoxEnabledButtons
  );
  connect(Settings::instance(), &Settings::settingsWritableChanged, this, &ServerConfigDialog::updateControls);

  connect(ui->btnAddPauseApp, &QPushButton::clicked, this, &ServerConfigDialog::addPauseApp);
  connect(ui->btnRemovePauseApp, &QPushButton::clicked, this, &ServerConfigDialog::removePauseApp);
  connect(ui->btnClearPauseApps, &QPushButton::clicked, this, &ServerConfigDialog::clearPauseApps);
  connect(ui->cbPauseApp, &QComboBox::activated, this, &ServerConfigDialog::addPauseApp);
  connect(ui->linePauseTitles, &QLineEdit::textChanged, this, &ServerConfigDialog::setButtonBoxEnabledButtons);
}

void ServerConfigDialog::updateControls() const
{
  const bool writable = Settings::isWritable();
  ui->cbDefaultLockToComputerState->setEnabled(writable);
  ui->cbDisableLockToComputer->setEnabled(writable);
  ui->cbEnableClipboard->setEnabled(writable);
  ui->sbClipboardSizeLimit->setEnabled(writable);
  ui->rbProtocolBarrier->setEnabled(writable);
  ui->rbProtocolSynergy->setEnabled(writable);
  ui->cbHeartbeat->setEnabled(writable);
  ui->cbRelativeMouseMoves->setEnabled(writable);
  ui->cbSwitchDelay->setEnabled(writable);
  ui->cbWin32KeepForeground->setEnabled(writable);
  ui->cbSwitchDoubleTap->setEnabled(writable);
  ui->sbSwitchDoubleTap->setEnabled(writable && ui->cbSwitchDoubleTap->isChecked());
  ui->sbSwitchDelay->setEnabled(writable && ui->cbSwitchDelay->isChecked());
  ui->groupExternalConfig->setEnabled(writable);
  ui->groupPauseOnFocus->setEnabled(writable);
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::restoreFromDefaults()
{
  m_protocol = networkProtocolFromString(Settings::defaultValue(Settings::Server::Protocol).toString());
  m_enableHeartbeat = Settings::defaultValue(Settings::Server::EnableHeartbeat).toBool();
  m_heartbeatRate = Settings::defaultValue(Settings::Server::Heartbeat).toInt();
  m_relativeMouseMoves = Settings::defaultValue(Settings::Server::RelativeMouseMoves).toBool();
  m_win32keepForeground = Settings::defaultValue(Settings::Server::Win32KeepForeground).toBool();
  m_enableSwitchDelay = Settings::defaultValue(Settings::Server::EnableSwitchDelay).toBool();
  m_switchDelay = Settings::defaultValue(Settings::Server::SwitchDelay).toInt();
  m_enableSwitchDoubleTap = Settings::defaultValue(Settings::Server::EnableSwitchDoubleTap).toBool();
  m_switchDoubleTap = Settings::defaultValue(Settings::Server::SwitchDoubleTap).toInt();
  m_defaultLockToComputerState = Settings::defaultValue(Settings::Server::DefaultLockToComputerState).toBool();
  m_disableLockToComputer = Settings::defaultValue(Settings::Server::DisableLockToComputer).toBool();
  m_enableClipboard = Settings::defaultValue(Settings::Server::EnableClipboard).toBool();
  m_clipboardSize = Settings::defaultValue(Settings::Server::ClipboardSize).toUInt();

  ui->groupExternalConfig->setChecked(Settings::defaultValue(Settings::Server::ExternalConfig).toBool());
  ui->lineConfigFile->setText(Settings::defaultValue(Settings::Server::ExternalConfigFile).toString());
  m_pauseApps = Settings::defaultValue(Settings::Server::PauseOnFocusApps).toStringList();
  ui->linePauseTitles->setText(Settings::defaultValue(Settings::Server::PauseOnFocusTitles).toStringList().join(", "));

  refreshControls();
  updateControls();
}

void ServerConfigDialog::setServerConfig()
{
  const auto configFile = ui->lineConfigFile->text();
  if (!QFile::exists(configFile)) {
    m_buttonBox->enableSave(false);
    return;
  }
  serverConfig().setConfigFile(configFile);
  setButtonBoxEnabledButtons();
}

bool ServerConfigDialog::addComputer(const QString &clientName, bool doSilent)
{
  bool isAccepted = false;
  Computer newComputer(clientName);

  if (ComputerSettingsDialog dlg(this, &newComputer, &model().m_computers);
      doSilent || dlg.exec() == QDialog::Accepted) {
    model().addComputer(newComputer);
    isAccepted = true;
  }

  ui->lblNewComputer->setEnabled(!model().isFull());
  return isAccepted;
}

bool ServerConfigDialog::isGeneralConfigModified() const
{
  return m_originalServerConfigIsExternal != Settings::value(Settings::Server::ExternalConfig).toBool() ||
         m_originalServerConfigUsesExternalFile != Settings::value(Settings::Server::ExternalConfigFile).toString() ||
         m_protocol != Settings::networkProtocol() ||
         m_enableClipboard != Settings::value(Settings::Server::EnableClipboard).toBool() ||
         m_clipboardSize != Settings::value(Settings::Server::ClipboardSize).toUInt() ||
         m_enableHeartbeat != Settings::value(Settings::Server::EnableHeartbeat).toBool() ||
         m_heartbeatRate != Settings::value(Settings::Server::Heartbeat).toInt() ||
         m_enableSwitchDelay != Settings::value(Settings::Server::EnableSwitchDelay).toBool() ||
         m_switchDelay != Settings::value(Settings::Server::SwitchDelay).toInt() ||
         m_enableSwitchDoubleTap != Settings::value(Settings::Server::EnableSwitchDoubleTap).toBool() ||
         m_switchDoubleTap != Settings::value(Settings::Server::SwitchDoubleTap).toInt() ||
         m_relativeMouseMoves != Settings::value(Settings::Server::RelativeMouseMoves).toBool() ||
         m_win32keepForeground != Settings::value(Settings::Server::Win32KeepForeground).toBool() ||
         m_disableLockToComputer != Settings::value(Settings::Server::DisableLockToComputer).toBool() ||
         m_defaultLockToComputerState != Settings::value(Settings::Server::DefaultLockToComputerState).toBool() ||
         m_pauseApps != Settings::value(Settings::Server::PauseOnFocusApps).toStringList() ||
         pauseTitles() != Settings::value(Settings::Server::PauseOnFocusTitles).toStringList();
}

bool ServerConfigDialog::isGeneralConfigDefault() const
{
  return ui->groupExternalConfig->isChecked() == Settings::defaultValue(Settings::Server::ExternalConfig).toBool() &&
         ui->lineConfigFile->text() == Settings::defaultValue(Settings::Server::ExternalConfigFile).toString() &&
         m_protocol == networkProtocolFromString(Settings::defaultValue(Settings::Server::Protocol).toString()) &&
         m_enableClipboard == Settings::defaultValue(Settings::Server::EnableClipboard).toBool() &&
         m_clipboardSize == Settings::defaultValue(Settings::Server::ClipboardSize).toUInt() &&
         m_enableHeartbeat == Settings::defaultValue(Settings::Server::EnableHeartbeat).toBool() &&
         m_heartbeatRate == Settings::defaultValue(Settings::Server::Heartbeat).toInt() &&
         m_enableSwitchDelay == Settings::defaultValue(Settings::Server::EnableSwitchDelay).toBool() &&
         m_switchDelay == Settings::defaultValue(Settings::Server::SwitchDelay).toInt() &&
         m_enableSwitchDoubleTap == Settings::defaultValue(Settings::Server::EnableSwitchDoubleTap).toBool() &&
         m_switchDoubleTap == Settings::defaultValue(Settings::Server::SwitchDoubleTap).toInt() &&
         m_relativeMouseMoves == Settings::defaultValue(Settings::Server::RelativeMouseMoves).toBool() &&
         m_win32keepForeground == Settings::defaultValue(Settings::Server::Win32KeepForeground).toBool() &&
         m_disableLockToComputer == Settings::defaultValue(Settings::Server::DisableLockToComputer).toBool() &&
         m_defaultLockToComputerState == Settings::defaultValue(Settings::Server::DefaultLockToComputerState).toBool() &&
         m_pauseApps == Settings::defaultValue(Settings::Server::PauseOnFocusApps).toStringList() &&
         pauseTitles() == Settings::defaultValue(Settings::Server::PauseOnFocusTitles).toStringList();
}

void ServerConfigDialog::setButtonBoxEnabledButtons() const
{
  const bool writable = Settings::isWritable();
  m_buttonBox->enableSave(writable && (isGeneralConfigModified() || !(m_originalServerConfig == m_serverConfig)));
  m_buttonBox->enableReset(writable && (isGeneralConfigModified() || !(m_originalServerConfig == m_serverConfig)));
  m_buttonBox->enableRestoreDefaults(writable && !isGeneralConfigDefault() && ui->tabWidget->currentIndex() == 2);
}

void ServerConfigDialog::addPauseApp()
{
  const auto index = ui->cbPauseApp->currentIndex();
  QString appName;
  if (index >= 0 && ui->cbPauseApp->currentText() == ui->cbPauseApp->itemText(index))
    appName = ui->cbPauseApp->itemData(index).toString();
  else
    appName = ui->cbPauseApp->currentText().trimmed();

  if (appName.isEmpty())
    return;

  if (!m_pauseApps.contains(appName, Qt::CaseInsensitive)) {
    m_pauseApps.append(appName);
    m_pauseApps.sort(Qt::CaseInsensitive);
  }
  refreshPauseAppList();
  ui->cbPauseApp->setCurrentIndex(-1);
  ui->cbPauseApp->setEditText(QString());
  ui->cbPauseApp->setFocus();
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::removePauseApp()
{
  const auto selectedItems = ui->listPauseApps->selectedItems();
  if (selectedItems.isEmpty())
    return;

  for (const auto *item : selectedItems)
    m_pauseApps.removeAll(item->data(Qt::UserRole).toString());
  refreshPauseAppList();
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::clearPauseApps()
{
  if (m_pauseApps.isEmpty())
    return;

  m_pauseApps.clear();
  refreshPauseAppList();
  setButtonBoxEnabledButtons();
}

void ServerConfigDialog::refreshPauseAppList()
{
  ui->listPauseApps->clear();
  for (const auto &app : m_pauseApps) {
    auto *item = new QListWidgetItem(friendlyAppLabel(app), ui->listPauseApps);
    // The configuration always stores the executable name, the label is only for the user.
    item->setData(Qt::UserRole, app);
    item->setToolTip(app);
  }

  const bool writable = Settings::isWritable();
  ui->btnRemovePauseApp->setEnabled(writable && !m_pauseApps.isEmpty());
  ui->btnClearPauseApps->setEnabled(writable && !m_pauseApps.isEmpty());
}

void ServerConfigDialog::populatePauseAppChoices()
{
  ui->cbPauseApp->clear();
  m_appDescriptions.clear();

  QList<RunningProcess> processes;
  for (const auto &process : runningProcesses()) {
    const auto key = process.m_name.toLower();
    if (!m_appDescriptions.contains(key) || m_appDescriptions.value(key).isEmpty())
      m_appDescriptions.insert(key, process.m_description);

    bool known = false;
    for (const auto &seen : processes) {
      if (seen.m_name.compare(process.m_name, Qt::CaseInsensitive) == 0) {
        known = true;
        break;
      }
    }
    if (!known)
      processes.append(process);
  }

  // The window with focus is the settings dialog itself while it is open, so it never counts
  // as the foreground application the user is interested in.
  QString foreground = foregroundProcessName();
  const auto ownName = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
  if (foreground.compare(ownName, Qt::CaseInsensitive) == 0)
    foreground.clear();

  auto addChoice = [this](const RunningProcess &process, const QString &prefix) {
    const auto label = process.m_description.isEmpty()
                           ? process.m_name
                           : QStringLiteral("%1 (%2)").arg(process.m_description, process.m_name);
    ui->cbPauseApp->addItem(prefix.isEmpty() ? label : QStringLiteral("%1 %2").arg(prefix, label), process.m_name);
  };

  // Like the task manager, the application that has focus is listed first, the rest is sorted by name.
  const bool hasForeground = !foreground.isEmpty();
  if (hasForeground) {
    for (const auto &process : processes) {
      if (process.m_name.compare(foreground, Qt::CaseInsensitive) == 0) {
        addChoice(process, tr("(current foreground)"));
        break;
      }
    }
  }

  std::sort(processes.begin(), processes.end(), [](const RunningProcess &left, const RunningProcess &right) {
    return left.m_name.compare(right.m_name, Qt::CaseInsensitive) < 0;
  });

  for (const auto &process : processes) {
    if (hasForeground && process.m_name.compare(foreground, Qt::CaseInsensitive) == 0)
      continue;
    addChoice(process, QString());
  }

  ui->cbPauseApp->setCurrentIndex(-1);
  ui->cbPauseApp->setEditText(QString());
  refreshPauseAppList();
}

QString ServerConfigDialog::friendlyAppLabel(const QString &app) const
{
  const auto description = m_appDescriptions.value(app.toLower());
  return description.isEmpty() ? app : QStringLiteral("%1 (%2)").arg(description, app);
}

QStringList ServerConfigDialog::pauseTitles() const
{
  QStringList titles;
  for (const auto &part : ui->linePauseTitles->text().split(QLatin1Char(','), Qt::SkipEmptyParts)) {
    const auto title = part.trimmed();
    if (!title.isEmpty())
      titles.append(title);
  }
  return titles;
}

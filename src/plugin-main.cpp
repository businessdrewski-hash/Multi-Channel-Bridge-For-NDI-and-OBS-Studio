// Copyright (C) 2026 NDI Multichannel Bridge contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "bridge-common.hpp"
#include "bridge-sources.hpp"
#include "control-dialog.hpp"
#include "multichannel-output.hpp"
#include "ndi-runtime.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QAction>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QTimer>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("ndi-multichannel-bridge", "en-US")

namespace {
QPointer<BridgeControlDialog> g_dialog;
QAction *g_action = nullptr;
obs_output_info g_output_info{};
obs_source_info g_video_source_info{};
obs_source_info g_audio_source_info{};

void frontend_event(obs_frontend_event event, void *)
{
  if (event == OBS_FRONTEND_EVENT_EXIT && g_dialog)
    g_dialog->stopOutput();
}
} // namespace

const char *obs_module_name()
{
  return "NDI Multichannel Bridge";
}

const char *obs_module_description()
{
  return "Sends one NDI video stream with two discrete stereo audio pairs and receives/splits it on one shared clock.";
}

bool obs_module_load()
{
  QString error;
  if (!ndi_runtime::load(&error)) {
    BRIDGE_LOG(LOG_ERROR, "%s", error.toUtf8().constData());
    QTimer::singleShot(0, [error] {
      QMessageBox::critical(static_cast<QWidget *>(obs_frontend_get_main_window()),
                            QStringLiteral("NDI Multichannel Bridge"), error);
    });
    return true; // Keep the module visible in logs rather than crashing OBS.
  }

  g_output_info = create_multichannel_output_info();
  g_video_source_info = create_bridge_video_source_info();
  g_audio_source_info = create_bridge_audio_source_info();
  obs_register_output(&g_output_info);
  obs_register_source(&g_video_source_info);
  obs_register_source(&g_audio_source_info);

  auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
  g_dialog = new BridgeControlDialog(mainWindow);
  g_action = static_cast<QAction *>(
      obs_frontend_add_tools_menu_qaction(obs_module_text("Menu.Open")));
  QObject::connect(g_action, &QAction::triggered, [] {
    if (!g_dialog)
      return;
    g_dialog->show();
    g_dialog->raise();
    g_dialog->activateWindow();
  });
  obs_frontend_add_event_callback(frontend_event, nullptr);

  BRIDGE_LOG(LOG_INFO, "Plugin loaded, version %s", PLUGIN_VERSION);
  return true;
}

void obs_module_unload()
{
  obs_frontend_remove_event_callback(frontend_event, nullptr);
  if (g_dialog) {
    g_dialog->stopOutput();
    delete g_dialog;
    g_dialog = nullptr;
  }
  ndi_runtime::unload();
  BRIDGE_LOG(LOG_INFO, "Plugin unloaded");
}

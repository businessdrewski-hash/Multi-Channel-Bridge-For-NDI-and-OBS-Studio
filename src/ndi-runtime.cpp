// Copyright (C) 2026 NDI Multichannel Bridge contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "ndi-runtime.hpp"
#include "bridge-common.hpp"

#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QStringList>

#include <memory>

namespace {
std::unique_ptr<QLibrary> g_library;
const NDIlib_v6 *g_api = nullptr;
QString g_loaded_path;

using ndi_v6_load_fn = const NDIlib_v6 *(*)();

QString library_name()
{
#ifdef NDILIB_LIBRARY_NAME
  return QString::fromUtf8(NDILIB_LIBRARY_NAME);
#elif defined(_WIN32)
  return QStringLiteral("Processing.NDI.Lib.x64.dll");
#elif defined(__APPLE__)
  return QStringLiteral("libndi.dylib");
#else
  return QStringLiteral("libndi.so.6");
#endif
}

QStringList candidate_paths()
{
  QStringList candidates;
  const QString name = library_name();

  // Let the OS loader search PATH / normal library locations first.
  candidates << name;

  const QStringList env_names = {
      QStringLiteral("NDI_RUNTIME_DIR_V6"),
      QStringLiteral("NDI_RUNTIME_DIR_V5"),
      QStringLiteral("NDI_RUNTIME_DIR")};
  for (const QString &env_name : env_names) {
    const QString dir = qEnvironmentVariable(env_name.toUtf8().constData());
    if (!dir.isEmpty())
      candidates << QDir(dir).filePath(name);
  }

#ifdef _WIN32
  candidates << QStringLiteral("C:/Program Files/NDI/NDI 6 Runtime/v6/") + name;
  candidates << QStringLiteral("C:/Program Files/NDI/NDI 5 Runtime/v5/") + name;
#else
  candidates << QStringLiteral("/usr/local/lib/") + name;
  candidates << QStringLiteral("/usr/lib/") + name;
  candidates << QStringLiteral("/usr/lib64/") + name;
#endif
  candidates.removeDuplicates();
  return candidates;
}
} // namespace

namespace ndi_runtime {

bool load(QString *error_message)
{
  if (g_api)
    return true;

  QStringList failures;
  for (const QString &candidate : candidate_paths()) {
    auto library = std::make_unique<QLibrary>(candidate);
    library->setLoadHints(QLibrary::ResolveAllSymbolsHint);
    if (!library->load()) {
      failures << QStringLiteral("%1: %2").arg(candidate, library->errorString());
      continue;
    }

    auto load_fn = reinterpret_cast<ndi_v6_load_fn>(library->resolve("NDIlib_v6_load"));
    if (!load_fn) {
      failures << QStringLiteral("%1: NDIlib_v6_load not exported").arg(candidate);
      library->unload();
      continue;
    }

    const NDIlib_v6 *api = load_fn();
    if (!api || !api->initialize()) {
      failures << QStringLiteral("%1: NDI runtime initialization failed").arg(candidate);
      library->unload();
      continue;
    }

    g_loaded_path = library->fileName();
    g_library = std::move(library);
    g_api = api;
    BRIDGE_LOG(LOG_INFO, "Loaded NDI runtime %s from %s", g_api->version(),
               g_loaded_path.toUtf8().constData());
    return true;
  }

  if (error_message) {
    *error_message = QStringLiteral("Could not load the NDI 6 runtime. Install DistroAV/NDI Runtime 6.3 or newer.\n\n%1")
                         .arg(failures.join(QStringLiteral("\n")));
  }
  return false;
}

void unload()
{
  if (g_api)
    g_api->destroy();
  g_api = nullptr;
  if (g_library) {
    g_library->unload();
    g_library.reset();
  }
  g_loaded_path.clear();
}

const NDIlib_v6 *api()
{
  return g_api;
}

QString loaded_path()
{
  return g_loaded_path;
}

} // namespace ndi_runtime

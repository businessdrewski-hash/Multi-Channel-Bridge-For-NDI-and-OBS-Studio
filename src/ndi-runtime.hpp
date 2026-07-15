#pragma once

#include <Processing.NDI.Lib.h>
#include <QString>

class QLibrary;

namespace ndi_runtime {

bool load(QString *error_message = nullptr);
void unload();
const NDIlib_v6 *api();
QString loaded_path();

} // namespace ndi_runtime

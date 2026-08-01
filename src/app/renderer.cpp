// SPDX-License-Identifier: GPL-3.0-only
#include "pu/renderer.hpp"

#include "pu/infra/platform.hpp"

namespace pu {

void SetupSignalHandler() { platform::SetupSignalHandler(); }

}  // namespace pu
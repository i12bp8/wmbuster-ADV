// wM-Buster ADV — one shared text buffer for the loop task (json, log lines).
// Only use it for work that completes before returning; callers that need
// the text later must copy it (the MQTT queue does).
// GPL-3.0
#pragma once

#include <stddef.h>

namespace wmb {

static const size_t SCRATCH_LEN = 4096;
char* scratch();

} // namespace wmb

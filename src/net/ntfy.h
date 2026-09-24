// wM-Buster ADV — ntfy.sh push notifications (queued, sent by a background
// task so HTTP never blocks the loop).
// GPL-3.0
#pragma once

namespace wmb {

void ntfy_begin();
void ntfy_notify(const char* title, const char* message);

} // namespace wmb

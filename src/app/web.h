// The HTTP server: the web UI (embedded, gzipped), the JSON API, the setup
// hotspot's pages, and the /library file manager API (same routes and JSON
// as the Pi frame's, so one client works against both).
#pragma once

namespace flapboard {
namespace web {

void begin();   // after net::begin() (needs the network stack up)

// Asked for by the browser, done by the main loop.
bool takeRebootRequest();

}  // namespace web
}  // namespace flapboard

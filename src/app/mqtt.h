// Home Assistant over MQTT, with discovery (no YAML), the same design as the
// Pi frame and the Lixie clocks: retained discovery republished on every
// connect, availability via LWT, QoS 1, state on change plus a 30 s heartbeat.
//
// Topics live under flapboard/<serial> (the serial, not the name, so renaming
// the sign never orphans its entities). Commands arrive on the MQTT task and
// are carried out by the main loop, which owns flash writes and I2C.
#pragma once

#include <string>

namespace flapboard {
namespace mqtt {

void begin();   // after net::begin()
void loop();    // main loop: applies commands, publishes state
std::string statusJson();
// From the web page; empty password = keep the stored one.
void setServer(const std::string &host, int port, const std::string &user, const std::string &password);

}  // namespace mqtt
}  // namespace flapboard

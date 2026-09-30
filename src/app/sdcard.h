// The microSD slot, mounted once at boot and kept mounted (from T48-for-Tab5).
// Mounting reconfigures the SDIO pins, so nothing mounts on demand.
#pragma once

#include <cstdint>

namespace flapboard {
namespace sdcard {

bool begin();            // safe to call again; picks up a card inserted later
bool mounted();
const char *mountPoint();   // "/sdcard"
const char *busWidth();     // "4-bit" / "1-bit" once mounted
uint64_t cardBytes();
void space(uint64_t *free_b, uint64_t *total_b);
// Repartitions the whole card and writes FAT32 (32 KB clusters). ERASES IT.
bool formatWholeCard();

}  // namespace sdcard
}  // namespace flapboard

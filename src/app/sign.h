// The split-flap board on the Tab5: flapcore drawing into the panel from its
// own task on core 1. Other tasks hand it messages; it never blocks them.
#pragma once

#include <string>

namespace flapboard {
namespace sign {

void begin();                              // after M5.begin(); starts the render task
// Any task: queue a message. align: 0 left, 1 centre, 2 right.
void show(const std::string &text, int align = 1, bool vertical_center = true);
std::string statsJson();                   // last frame timings, for /api/status
void runBench();                           // serial 'bench': time the drawing steps

// The quick panel (long press anywhere): volume, mute, the sign's address.
// Touch is read by the main loop and handed over; the render task draws.
void openPanel();
// Info-sheet buttons that change settings, carried out by the main loop.
enum class ActionKind { Brightness, Source, Next, ShowIp };
struct Action {
  ActionKind kind;
  int arg;
};
bool takeAction(Action *a);
const char *sourceForMode(int i);
// Display off (sleep hours, "off" from Home Assistant): stop drawing; on
// again: redraw everything. The board keeps time meanwhile.
void setActive(bool active);

// Photo mode (Phase 9): show a library photo ("photos/x.jpg") full screen with
// the configured transition; the board is set aside until showBoard().
void showPhoto(const std::string &rel);
void showBoard();
bool photoMode();
void photoCaption(const std::string &text);   // a message over the photo ("" = none)
bool takePhotoFailure(std::string *rel);      // a photo that could not be read
bool panelOpen();
void panelTap(int x, int y);

}  // namespace sign
}  // namespace flapboard

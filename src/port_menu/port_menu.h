// Re:Fine 6b+6c: the port's settings menu (F1). See port_menu.cpp.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Once, at boot, before the frame clock starts: sets the runtime's overlay hooks and loads
// RmlUi and the fonts on a thread of their own. The values are shown read-only.
void port_menu_start(int render_height, int fps_target);

#ifdef __cplusplus
}
#endif

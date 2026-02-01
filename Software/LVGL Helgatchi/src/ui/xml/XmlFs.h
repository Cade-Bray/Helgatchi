#pragma once

#include <lvgl.h>

// Initialize a minimal LVGL filesystem driver backed by Arduino LittleFS.
// Returns true if LittleFS was mounted and the driver registered.
bool xml_fs_init();

// LVGL drive letter used by the XML loader (e.g. "S:/ui").
char xml_fs_drive_letter();

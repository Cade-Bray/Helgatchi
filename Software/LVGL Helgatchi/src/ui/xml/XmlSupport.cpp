#include "XmlSupport.h"

#include <Arduino.h>

#include "XmlFs.h"

#if LV_USE_XML
#include <lvgl.h>
#include "src/others/xml/lv_xml_component.h"
#endif

namespace {
bool s_xml_ready = false;
}

bool xml_support_init() {
#if !LV_USE_XML
  Serial.println("[xml] LV_USE_XML is disabled in lv_conf.h");
  return false;
#else
  if (s_xml_ready) return true;

  if (!xml_fs_init()) {
    Serial.println("[xml] filesystem init failed; XML loading disabled");
    return false;
  }

  lv_xml_init();
  s_xml_ready = true;
  Serial.println("[xml] LVGL XML initialized");
  return true;
#endif
}

bool xml_support_load_all(const char *path) {
#if !LV_USE_XML
  (void)path;
  return false;
#else
  if (!s_xml_ready) return false;
  if (!path || path[0] == '\0') return false;

  // Load globals.xml first if it exists.
  char globals_path[64];
  snprintf(globals_path, sizeof(globals_path), "%s/globals.xml", path);
  lv_fs_file_t globals_file{};
  if (lv_fs_open(&globals_file, globals_path, LV_FS_MODE_RD) == LV_FS_RES_OK) {
    lv_fs_close(&globals_file);
    lv_xml_register_component_from_file(globals_path);
  }

  // Load standard folders used by the LVGL Editor.
  const char *folders[] = {"components", "widgets", "screens"};
  for (const char *folder : folders) {
    char folder_path[64];
    snprintf(folder_path, sizeof(folder_path), "%s/%s", path, folder);

    lv_fs_dir_t dir{};
    if (lv_fs_dir_open(&dir, folder_path) == LV_FS_RES_OK) {
      lv_fs_dir_close(&dir);
      lv_result_t res = lv_xml_load_all_from_path(folder_path);
      if (res != LV_RESULT_OK) {
        Serial.printf("[xml] load failed for path: %s\n", folder_path);
      }
    }
  }

  Serial.printf("[xml] loaded XML assets from: %s\n", path);
  return true;
#endif
}

lv_obj_t *xml_support_try_create_screen(const char *name) {
#if !LV_USE_XML
  (void)name;
  return nullptr;
#else
  if (!s_xml_ready || !name) return nullptr;
  return lv_xml_create_screen(name);
#endif
}

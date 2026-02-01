#include "XmlFs.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>

namespace {
constexpr char kDriveLetter = 'S';
static bool s_fs_ready = false;
static lv_fs_drv_t s_fs_drv;

struct FsFileHandle {
  File file;
};

struct FsDirHandle {
  File dir;
};

static bool fs_ready_cb(lv_fs_drv_t *) {
  return s_fs_ready;
}

static void *fs_open(lv_fs_drv_t *, const char *path, lv_fs_mode_t mode) {
  if (!s_fs_ready) return nullptr;

  const char *open_mode = (mode & LV_FS_MODE_WR) ? ((mode & LV_FS_MODE_RD) ? "r+" : "w") : "r";
  File f = LittleFS.open(path, open_mode);
  if (!f) return nullptr;

  auto *handle = new FsFileHandle{f};
  return handle;
}

static lv_fs_res_t fs_close(lv_fs_drv_t *, void *file_p) {
  if (!file_p) return LV_FS_RES_INV_PARAM;
  auto *handle = static_cast<FsFileHandle *>(file_p);
  handle->file.close();
  delete handle;
  return LV_FS_RES_OK;
}

static lv_fs_res_t fs_read(lv_fs_drv_t *, void *file_p, void *buf, uint32_t btr, uint32_t *br) {
  if (!file_p || !buf) return LV_FS_RES_INV_PARAM;
  auto *handle = static_cast<FsFileHandle *>(file_p);
  const size_t read = handle->file.read(static_cast<uint8_t *>(buf), btr);
  if (br) *br = static_cast<uint32_t>(read);
  return LV_FS_RES_OK;
}

static lv_fs_res_t fs_write(lv_fs_drv_t *, void *file_p, const void *buf, uint32_t btw, uint32_t *bw) {
  if (!file_p || !buf) return LV_FS_RES_INV_PARAM;
  auto *handle = static_cast<FsFileHandle *>(file_p);
  const size_t written = handle->file.write(static_cast<const uint8_t *>(buf), btw);
  if (bw) *bw = static_cast<uint32_t>(written);
  return LV_FS_RES_OK;
}

static lv_fs_res_t fs_seek(lv_fs_drv_t *, void *file_p, uint32_t pos, lv_fs_whence_t whence) {
  if (!file_p) return LV_FS_RES_INV_PARAM;
  auto *handle = static_cast<FsFileHandle *>(file_p);
  SeekMode mode = SeekSet;
  if (whence == LV_FS_SEEK_CUR) mode = SeekCur;
  else if (whence == LV_FS_SEEK_END) mode = SeekEnd;
  const bool ok = handle->file.seek(pos, mode);
  return ok ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t fs_tell(lv_fs_drv_t *, void *file_p, uint32_t *pos_p) {
  if (!file_p || !pos_p) return LV_FS_RES_INV_PARAM;
  auto *handle = static_cast<FsFileHandle *>(file_p);
  *pos_p = static_cast<uint32_t>(handle->file.position());
  return LV_FS_RES_OK;
}

static void *fs_dir_open(lv_fs_drv_t *, const char *path) {
  if (!s_fs_ready) return nullptr;
  File dir = LittleFS.open(path);
  if (!dir || !dir.isDirectory()) return nullptr;
  auto *handle = new FsDirHandle{dir};
  return handle;
}

static lv_fs_res_t fs_dir_read(lv_fs_drv_t *, void *rddir_p, char *fn, uint32_t fn_len) {
  if (!rddir_p || !fn || fn_len == 0) return LV_FS_RES_INV_PARAM;
  auto *handle = static_cast<FsDirHandle *>(rddir_p);

  File entry = handle->dir.openNextFile();
  if (!entry) {
    fn[0] = '\0';
    return LV_FS_RES_OK;
  }

  const char *name = entry.name();
  if (!name) {
    fn[0] = '\0';
    return LV_FS_RES_OK;
  }

  // LVGL expects directories to be returned with a leading '/'.
  if (entry.isDirectory()) {
    if (fn_len >= 2) {
      fn[0] = '/';
      strncpy(fn + 1, name, fn_len - 1);
      fn[fn_len - 1] = '\0';
    } else {
      fn[0] = '\0';
    }
  } else {
    strncpy(fn, name, fn_len);
    fn[fn_len - 1] = '\0';
  }

  entry.close();
  return LV_FS_RES_OK;
}

static lv_fs_res_t fs_dir_close(lv_fs_drv_t *, void *rddir_p) {
  if (!rddir_p) return LV_FS_RES_INV_PARAM;
  auto *handle = static_cast<FsDirHandle *>(rddir_p);
  handle->dir.close();
  delete handle;
  return LV_FS_RES_OK;
}
}

bool xml_fs_init() {
  if (s_fs_ready) return true;

  if (!LittleFS.begin()) {
    Serial.println("[xml] LittleFS mount failed");
    s_fs_ready = false;
    return false;
  }

  lv_fs_drv_init(&s_fs_drv);
  s_fs_drv.letter = kDriveLetter;
  s_fs_drv.cache_size = 0;
  s_fs_drv.ready_cb = fs_ready_cb;
  s_fs_drv.open_cb = fs_open;
  s_fs_drv.close_cb = fs_close;
  s_fs_drv.read_cb = fs_read;
  s_fs_drv.write_cb = fs_write;
  s_fs_drv.seek_cb = fs_seek;
  s_fs_drv.tell_cb = fs_tell;
  s_fs_drv.dir_open_cb = fs_dir_open;
  s_fs_drv.dir_read_cb = fs_dir_read;
  s_fs_drv.dir_close_cb = fs_dir_close;
  lv_fs_drv_register(&s_fs_drv);

  s_fs_ready = true;
  Serial.println("[xml] LittleFS mounted and LVGL FS driver registered");
  return true;
}

char xml_fs_drive_letter() {
  return kDriveLetter;
}

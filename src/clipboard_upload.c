#include "clipboard_upload.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifndef _WIN32
#include <dirent.h>
#include <unistd.h>
#endif

static void set_error(char *error, size_t error_size, const char *format, ...) {
  if (error == NULL || error_size == 0) return;
  va_list args;
  va_start(args, format);
  vsnprintf(error, error_size, format, args);
  va_end(args);
}

void clipboard_upload_init(clipboard_upload_t *upload) {
  memset(upload, 0, sizeof(*upload));
  upload->dir_fd = -1;
  upload->file_fd = -1;
}

bool clipboard_upload_active(const clipboard_upload_t *upload) { return upload->file_fd >= 0; }

bool clipboard_upload_parse_ttl(const char *value, time_t *ttl_seconds) {
  if (value == NULL || ttl_seconds == NULL || value[0] == '\0' || value[0] == '-') return false;

  char *endptr;
  errno = 0;
  uintmax_t parsed = strtoumax(value, &endptr, 10);
  if (errno != 0 || endptr == value || *endptr != '\0' || parsed == 0) return false;

  time_t converted = (time_t)parsed;
  if (converted <= 0 || (uintmax_t)converted != parsed) return false;
  *ttl_seconds = converted;
  return true;
}

#ifdef _WIN32

bool clipboard_upload_prepare_directory(const char *directory, char *error, size_t error_size) {
  (void)directory;
  set_error(error, error_size, "clipboard image upload is not supported on Windows");
  return false;
}

void clipboard_upload_prune(const char *directory, time_t ttl_seconds) {
  (void)directory;
  (void)ttl_seconds;
}

bool clipboard_upload_start(clipboard_upload_t *upload, const char *directory, size_t max_size,
                            size_t expected_size, const char *mime, const uint8_t random_bytes[8], char *error,
                            size_t error_size) {
  (void)upload;
  (void)directory;
  (void)max_size;
  (void)expected_size;
  (void)mime;
  (void)random_bytes;
  set_error(error, error_size, "clipboard image upload is not supported on Windows");
  return false;
}

bool clipboard_upload_write(clipboard_upload_t *upload, const void *data, size_t size, char *error,
                            size_t error_size) {
  (void)upload;
  (void)data;
  (void)size;
  set_error(error, error_size, "clipboard image upload is not supported on Windows");
  return false;
}

bool clipboard_upload_finish(clipboard_upload_t *upload, char *path, size_t path_size, char *error,
                             size_t error_size) {
  (void)upload;
  (void)path;
  (void)path_size;
  set_error(error, error_size, "clipboard image upload is not supported on Windows");
  return false;
}

void clipboard_upload_abort(clipboard_upload_t *upload) { clipboard_upload_init(upload); }

#else

static bool is_directory(const char *path) {
  struct stat st;
  return lstat(path, &st) == 0 && S_ISDIR(st.st_mode) && !S_ISLNK(st.st_mode);
}

bool clipboard_upload_prepare_directory(const char *directory, char *error, size_t error_size) {
  if (directory == NULL || directory[0] != '/' || strcmp(directory, "/") == 0) {
    set_error(error, error_size, "clipboard upload directory must be an absolute path");
    return false;
  }
  if (strlen(directory) + CLIPBOARD_UPLOAD_NAME_MAX + 1 >= CLIPBOARD_UPLOAD_PATH_MAX) {
    set_error(error, error_size, "clipboard upload directory is too long");
    return false;
  }

  char path[CLIPBOARD_UPLOAD_PATH_MAX];
  snprintf(path, sizeof(path), "%s", directory);
  for (char *cursor = path + 1; *cursor != '\0'; cursor++) {
    if (*cursor != '/') continue;
    *cursor = '\0';
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
      set_error(error, error_size, "cannot create clipboard upload directory: %s", strerror(errno));
      return false;
    }
    if (!is_directory(path)) {
      set_error(error, error_size, "clipboard upload path component is not a directory");
      return false;
    }
    *cursor = '/';
  }
  bool created = mkdir(path, 0700) == 0;
  if (!created && errno != EEXIST) {
    set_error(error, error_size, "cannot create clipboard upload directory: %s", strerror(errno));
    return false;
  }
  if (!is_directory(path)) {
    set_error(error, error_size, "clipboard upload path is not a directory");
    return false;
  }
  if (created && chmod(path, 0700) != 0) {
    set_error(error, error_size, "cannot protect clipboard upload directory: %s", strerror(errno));
    return false;
  }
  struct stat st;
  if (lstat(path, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 0777) != 0700) {
    set_error(error, error_size, "clipboard upload directory must be owned by the service user with mode 0700");
    return false;
  }
  return true;
}

void clipboard_upload_prune(const char *directory, time_t ttl_seconds) {
  if (directory == NULL || ttl_seconds <= 0) return;
  int dir_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (dir_fd < 0) return;
  DIR *dir = fdopendir(dir_fd);
  if (dir == NULL) {
    close(dir_fd);
    return;
  }

  time_t cutoff = time(NULL) - ttl_seconds;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, "clipboard-", 10) != 0 && strncmp(entry->d_name, ".clipboard-", 11) != 0)
      continue;
    struct stat st;
    if (fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(st.st_mode)) continue;
    if (st.st_mtime < cutoff) unlinkat(dir_fd, entry->d_name, 0);
  }
  closedir(dir);
}

static const clipboard_image_type_t *image_type_for_mime(const char *mime) {
  for (size_t i = 0; i < clipboard_image_type_count; i++) {
    if (strcmp(mime, clipboard_image_types[i].mime) == 0) return &clipboard_image_types[i];
  }
  return NULL;
}

static const char *signature_extension(const unsigned char *data, size_t size) {
  for (size_t type_index = 0; type_index < clipboard_image_type_count; type_index++) {
    const clipboard_image_type_t *image_type = &clipboard_image_types[type_index];
    for (size_t variant_index = 0; variant_index < image_type->variant_count; variant_index++) {
      const clipboard_signature_variant_t *variant = &image_type->variants[variant_index];
      bool matches = true;
      for (size_t part_index = 0; part_index < variant->part_count; part_index++) {
        const clipboard_magic_part_t *part = &variant->parts[part_index];
        if (part->offset + part->length > size || memcmp(data + part->offset, part->bytes, part->length) != 0) {
          matches = false;
          break;
        }
      }
      if (matches) return image_type->extension;
    }
  }
  return NULL;
}

bool clipboard_upload_start(clipboard_upload_t *upload, const char *directory, size_t max_size,
                            size_t expected_size, const char *mime, const uint8_t random_bytes[8], char *error,
                            size_t error_size) {
  if (clipboard_upload_active(upload)) {
    set_error(error, error_size, "an image upload is already in progress");
    return false;
  }
  if (expected_size == 0 || expected_size > max_size) {
    set_error(error, error_size, "image size must be between 1 and %zu bytes", max_size);
    return false;
  }
  const clipboard_image_type_t *image_type = image_type_for_mime(mime);
  if (image_type == NULL) {
    set_error(error, error_size, "unsupported image type");
    return false;
  }

  int dir_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (dir_fd < 0) {
    set_error(error, error_size, "cannot open clipboard upload directory: %s", strerror(errno));
    return false;
  }

  char id[17];
  for (size_t i = 0; i < 8; i++) snprintf(id + (i * 2), 3, "%02x", random_bytes[i]);
  snprintf(upload->temp_name, sizeof(upload->temp_name), ".clipboard-%s.part", id);
  snprintf(upload->final_name, sizeof(upload->final_name), "clipboard-%s.%s", id, image_type->extension);

  int file_fd = openat(dir_fd, upload->temp_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (file_fd < 0) {
    set_error(error, error_size, "cannot create clipboard image: %s", strerror(errno));
    close(dir_fd);
    return false;
  }

  upload->dir_fd = dir_fd;
  upload->file_fd = file_fd;
  upload->expected_size = expected_size;
  upload->received_size = 0;
  upload->signature_size = 0;
  snprintf(upload->mime, sizeof(upload->mime), "%s", mime);
  snprintf(upload->final_path, sizeof(upload->final_path), "%s/%s", directory, upload->final_name);
  return true;
}

bool clipboard_upload_write(clipboard_upload_t *upload, const void *data, size_t size, char *error,
                            size_t error_size) {
  if (!clipboard_upload_active(upload)) {
    set_error(error, error_size, "no image upload is in progress");
    return false;
  }
  if (size > upload->expected_size - upload->received_size) {
    set_error(error, error_size, "image upload exceeded its declared size");
    return false;
  }

  size_t signature_remaining = sizeof(upload->signature) - upload->signature_size;
  size_t signature_copy = size < signature_remaining ? size : signature_remaining;
  if (signature_copy > 0) {
    memcpy(upload->signature + upload->signature_size, data, signature_copy);
    upload->signature_size += signature_copy;
  }

  const unsigned char *cursor = data;
  size_t remaining = size;
  while (remaining > 0) {
    ssize_t written = write(upload->file_fd, cursor, remaining);
    if (written < 0) {
      if (errno == EINTR) continue;
      set_error(error, error_size, "cannot write clipboard image: %s", strerror(errno));
      return false;
    }
    cursor += written;
    remaining -= (size_t)written;
  }
  upload->received_size += size;
  return true;
}

bool clipboard_upload_finish(clipboard_upload_t *upload, char *path, size_t path_size, char *error,
                             size_t error_size) {
  if (!clipboard_upload_active(upload)) {
    set_error(error, error_size, "no image upload is in progress");
    return false;
  }
  if (upload->received_size != upload->expected_size) {
    set_error(error, error_size, "image upload is incomplete");
    return false;
  }

  const clipboard_image_type_t *declared_type = image_type_for_mime(upload->mime);
  const char *detected_extension = signature_extension(upload->signature, upload->signature_size);
  if (declared_type == NULL || detected_extension == NULL || strcmp(declared_type->extension, detected_extension) != 0) {
    set_error(error, error_size, "image content does not match its declared type");
    return false;
  }
  if (fsync(upload->file_fd) != 0) {
    set_error(error, error_size, "cannot sync clipboard image: %s", strerror(errno));
    return false;
  }
  if (close(upload->file_fd) != 0) {
    upload->file_fd = -1;
    set_error(error, error_size, "cannot close clipboard image: %s", strerror(errno));
    return false;
  }
  upload->file_fd = -1;
  if (renameat(upload->dir_fd, upload->temp_name, upload->dir_fd, upload->final_name) != 0) {
    set_error(error, error_size, "cannot finalize clipboard image: %s", strerror(errno));
    return false;
  }
  if (snprintf(path, path_size, "%s", upload->final_path) >= (int)path_size) {
    unlinkat(upload->dir_fd, upload->final_name, 0);
    set_error(error, error_size, "clipboard image path is too long");
    return false;
  }
  close(upload->dir_fd);
  clipboard_upload_init(upload);
  return true;
}

void clipboard_upload_abort(clipboard_upload_t *upload) {
  if (upload->file_fd >= 0) close(upload->file_fd);
  if (upload->dir_fd >= 0 && upload->temp_name[0] != '\0') unlinkat(upload->dir_fd, upload->temp_name, 0);
  if (upload->dir_fd >= 0) close(upload->dir_fd);
  clipboard_upload_init(upload);
}

#endif

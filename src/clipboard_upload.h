#ifndef TTYD_CLIPBOARD_UPLOAD_H
#define TTYD_CLIPBOARD_UPLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "clipboard_protocol.generated.h"

#define CLIPBOARD_UPLOAD_PATH_MAX 4096
#define CLIPBOARD_UPLOAD_NAME_MAX 96

typedef struct {
  int dir_fd;
  int file_fd;
  size_t expected_size;
  size_t received_size;
  unsigned char signature[CLIPBOARD_UPLOAD_SIGNATURE_SIZE];
  size_t signature_size;
  char mime[CLIPBOARD_UPLOAD_MIME_MAX];
  char temp_name[CLIPBOARD_UPLOAD_NAME_MAX];
  char final_name[CLIPBOARD_UPLOAD_NAME_MAX];
  char final_path[CLIPBOARD_UPLOAD_PATH_MAX];
} clipboard_upload_t;

void clipboard_upload_init(clipboard_upload_t *upload);
bool clipboard_upload_prepare_directory(const char *directory, char *error, size_t error_size);
void clipboard_upload_prune(const char *directory, time_t ttl_seconds);
bool clipboard_upload_start(clipboard_upload_t *upload, const char *directory, size_t max_size,
                            size_t expected_size, const char *mime, const uint8_t random_bytes[8], char *error,
                            size_t error_size);
bool clipboard_upload_write(clipboard_upload_t *upload, const void *data, size_t size, char *error,
                            size_t error_size);
bool clipboard_upload_finish(clipboard_upload_t *upload, char *path, size_t path_size, char *error,
                             size_t error_size);
void clipboard_upload_abort(clipboard_upload_t *upload);
bool clipboard_upload_active(const clipboard_upload_t *upload);

#endif

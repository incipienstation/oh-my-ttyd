#include "clipboard_upload.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK(expression)                                                                           \
  do {                                                                                              \
    if (!(expression)) {                                                                            \
      fprintf(stderr, "CHECK failed at %s:%d: %s\n", __FILE__, __LINE__, #expression);            \
      exit(EXIT_FAILURE);                                                                           \
    }                                                                                               \
  } while (0)

static const unsigned char png[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
static const unsigned char jpeg[] = {0xff, 0xd8, 0xff};
static const unsigned char webp[] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'};
static const unsigned char gif87[] = {'G', 'I', 'F', '8', '7', 'a'};
static const unsigned char gif89[] = {'G', 'I', 'F', '8', '9', 'a'};

static void test_ttl_parsing(void) {
  time_t ttl = 0;
  CHECK(clipboard_upload_parse_ttl("1", &ttl));
  CHECK(ttl == 1);
  CHECK(clipboard_upload_parse_ttl("86400", &ttl));
  CHECK(ttl == 86400);
  CHECK(!clipboard_upload_parse_ttl("0", &ttl));
  CHECK(!clipboard_upload_parse_ttl("-1", &ttl));
  CHECK(!clipboard_upload_parse_ttl("1s", &ttl));
  CHECK(!clipboard_upload_parse_ttl("18446744073709551615", &ttl));
  CHECK(!clipboard_upload_parse_ttl("18446744073709551616", &ttl));
}

static void test_format(const char *directory, const char *mime, const char *extension,
                        const unsigned char *data, size_t size, uint8_t id) {
  clipboard_upload_t upload;
  clipboard_upload_init(&upload);
  const uint8_t random_bytes[8] = {id, id, id, id, id, id, id, id};
  char error[256] = "";
  char path[CLIPBOARD_UPLOAD_PATH_MAX];

  CHECK(clipboard_upload_start(&upload, directory, 1024, size, mime, random_bytes, error, sizeof(error)));
  CHECK(clipboard_upload_write(&upload, data, size, error, sizeof(error)));
  CHECK(clipboard_upload_finish(&upload, path, sizeof(path), error, sizeof(error)));
  char suffix[16];
  snprintf(suffix, sizeof(suffix), ".%s", extension);
  CHECK(strstr(path, suffix) != NULL);
  CHECK(unlink(path) == 0);
}

static void test_valid_upload(const char *directory) {
  clipboard_upload_t upload;
  clipboard_upload_init(&upload);
  const uint8_t random_bytes[8] = {0, 1, 2, 3, 4, 5, 6, 7};
  char error[256] = "";
  char path[CLIPBOARD_UPLOAD_PATH_MAX];

  CHECK(clipboard_upload_start(&upload, directory, 1024, sizeof(png), "image/png", random_bytes, error,
                               sizeof(error)));
  CHECK(clipboard_upload_write(&upload, png, 3, error, sizeof(error)));
  CHECK(clipboard_upload_write(&upload, png + 3, sizeof(png) - 3, error, sizeof(error)));
  CHECK(clipboard_upload_finish(&upload, path, sizeof(path), error, sizeof(error)));
  CHECK(strstr(path, "clipboard-0001020304050607.png") != NULL);

  struct stat st;
  CHECK(stat(path, &st) == 0);
  CHECK((st.st_mode & 0777) == 0600);
  CHECK(st.st_size == (off_t)sizeof(png));
  CHECK(unlink(path) == 0);

  test_format(directory, "image/jpeg", "jpg", jpeg, sizeof(jpeg), 1);
  test_format(directory, "image/webp", "webp", webp, sizeof(webp), 2);
  test_format(directory, "image/gif", "gif", gif87, sizeof(gif87), 3);
  test_format(directory, "image/gif", "gif", gif89, sizeof(gif89), 4);
}

static void test_rejections(const char *directory) {
  clipboard_upload_t upload;
  clipboard_upload_init(&upload);
  const uint8_t random_bytes[8] = {8, 9, 10, 11, 12, 13, 14, 15};
  char error[256] = "";
  char path[CLIPBOARD_UPLOAD_PATH_MAX];

  CHECK(!clipboard_upload_start(&upload, directory, 4, sizeof(png), "image/png", random_bytes, error,
                                sizeof(error)));
  CHECK(strstr(error, "between 1 and 4") != NULL);

  CHECK(!clipboard_upload_start(&upload, directory, 1024, sizeof(png), "image/bmp", random_bytes, error,
                                sizeof(error)));
  CHECK(strstr(error, "unsupported image type") != NULL);

  CHECK(clipboard_upload_start(&upload, directory, 1024, 3, "image/jpeg", random_bytes, error,
                               sizeof(error)));
  CHECK(clipboard_upload_write(&upload, "bad", 3, error, sizeof(error)));
  CHECK(!clipboard_upload_finish(&upload, path, sizeof(path), error, sizeof(error)));
  CHECK(strstr(error, "does not match") != NULL);
  clipboard_upload_abort(&upload);

  CHECK(clipboard_upload_start(&upload, directory, 1024, 2, "image/png", random_bytes, error,
                               sizeof(error)));
  CHECK(!clipboard_upload_write(&upload, png, 3, error, sizeof(error)));
  CHECK(strstr(error, "exceeded") != NULL);
  clipboard_upload_abort(&upload);
}

static void test_prune(const char *directory) {
  char *path = NULL;
  CHECK(asprintf(&path, "%s/clipboard-expired.png", directory) > 0);
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  CHECK(fd >= 0);
  close(fd);

  struct timespec times[2] = {{.tv_sec = time(NULL) - 120, .tv_nsec = 0},
                             {.tv_sec = time(NULL) - 120, .tv_nsec = 0}};
  CHECK(utimensat(AT_FDCWD, path, times, 0) == 0);
  clipboard_upload_prune(directory, 60);
  CHECK(access(path, F_OK) != 0);
  free(path);
}

int main(void) {
  test_ttl_parsing();

  char root[] = "/tmp/oh-my-ttyd-upload-test-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  char directory[CLIPBOARD_UPLOAD_PATH_MAX];
  snprintf(directory, sizeof(directory), "%s/nested/clipboard", root);

  char error[256] = "";
  CHECK(clipboard_upload_prepare_directory(directory, error, sizeof(error)));
  struct stat st;
  CHECK(stat(directory, &st) == 0);
  CHECK((st.st_mode & 0777) == 0700);

  CHECK(chmod(directory, 0750) == 0);
  CHECK(!clipboard_upload_prepare_directory(directory, error, sizeof(error)));
  CHECK(strstr(error, "mode 0700") != NULL);
  CHECK(chmod(directory, 0700) == 0);
  CHECK(clipboard_upload_prepare_directory(directory, error, sizeof(error)));

  test_valid_upload(directory);
  test_rejections(directory);
  test_prune(directory);

  char nested[CLIPBOARD_UPLOAD_PATH_MAX];
  snprintf(nested, sizeof(nested), "%s/nested", root);
  CHECK(rmdir(directory) == 0);
  CHECK(rmdir(nested) == 0);
  CHECK(rmdir(root) == 0);
  puts("clipboard upload tests passed");
  return 0;
}

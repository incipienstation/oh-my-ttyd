#include <assert.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wizard.h"

static int run_wizard_script(const char *script, struct wizard_output *out) {
  char path[] = "/tmp/ttyd-wizard-test-XXXXXX";
  int fd = mkstemp(path);
  assert(fd >= 0);

  size_t n = strlen(script);
  ssize_t written = write(fd, script, n);
  assert(written == (ssize_t)n);
  assert(lseek(fd, 0, SEEK_SET) == 0);

  int saved_stdin = dup(STDIN_FILENO);
  assert(saved_stdin >= 0);
  assert(dup2(fd, STDIN_FILENO) >= 0);

  int rc = wizard_interactive("ttyd", out);

  assert(dup2(saved_stdin, STDIN_FILENO) >= 0);
  close(saved_stdin);
  close(fd);
  unlink(path);
  return rc;
}

static void assert_argv(const struct wizard_output *out, const char **expected, int expected_len) {
  assert(out->argc == expected_len);
  for (int i = 0; i < expected_len; i++) {
    assert(strcmp(out->argv[i], expected[i]) == 0);
  }
}

static void test_defaults_print_action(void) {
  // command, interface, port, base path, writable, auth, tls, max clients, once, exit-no-conn, browser, action
  const char *script = "sh\n127.0.0.1\n7681\n\nn\nn\nn\n0\nn\nn\nn\n2\n";
  struct wizard_output out = {0};

  int rc = run_wizard_script(script, &out);
  assert(rc == 0);
  assert(out.action == WIZARD_ACTION_PRINT);

  const char *expected[] = {"ttyd", "-i", "127.0.0.1", "-p", "7681", "sh"};
  assert_argv(&out, expected, (int)(sizeof(expected) / sizeof(expected[0])));
  assert(strcmp(out.command_preview, "ttyd -i 127.0.0.1 -p 7681 sh") == 0);

  wizard_output_free(&out);
}

static void test_extended_options_print_action(void) {
  const char *script = "sh -c 'echo hi'\n0.0.0.0\n8888\n/demo/\ny\nn\nn\n5\ny\ny\ny\n2\n";
  struct wizard_output out = {0};

  int rc = run_wizard_script(script, &out);
  assert(rc == 0);
  assert(out.action == WIZARD_ACTION_PRINT);

  const char *expected[] = {
      "ttyd", "-i", "0.0.0.0", "-p", "8888", "-b", "/demo", "-W", "-m", "5", "-o", "-q", "-B", "sh", "-c", "echo hi",
  };
  assert_argv(&out, expected, (int)(sizeof(expected) / sizeof(expected[0])));

  wizard_output_free(&out);
}

int main(void) {
  setenv("SHELL", "/bin/sh", 1);

  test_defaults_print_action();
  test_extended_options_print_action();

  puts("wizard_harness: all tests passed");
  return 0;
}

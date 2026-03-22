#include "wizard.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libwebsockets.h>

#include "utils.h"

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define ACCESS _access
#define X_OK 0
#else
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>
#define ACCESS access
#endif

#define INPUT_BUFSZ 4096

struct wizard_config {
  char *command_line;
  char *interface;
  int port;
  char *base_path;
  bool writable;
  bool use_auth;
  char *username;
  char *password;
  bool use_tls;
  char *cert_path;
  char *key_path;
  char *ca_path;
  int max_clients;
  bool once;
  bool exit_no_conn;
  bool open_browser;
};

struct arg_builder {
  char **items;
  int len;
  int cap;
};

static void wizard_config_free(struct wizard_config *cfg) {
  if (cfg == NULL) return;
  free(cfg->command_line);
  free(cfg->interface);
  free(cfg->base_path);
  free(cfg->username);
  free(cfg->password);
  free(cfg->cert_path);
  free(cfg->key_path);
  free(cfg->ca_path);
}

void wizard_output_free(struct wizard_output *output) {
  if (output == NULL) return;
  free(output->command_preview);
  if (output->argv != NULL) {
    for (int i = 0; i < output->argc; i++) free(output->argv[i]);
    free(output->argv);
  }
  memset(output, 0, sizeof(*output));
}

static char *xstrdup(const char *s) {
  size_t n = strlen(s);
  char *dup = xmalloc(n + 1);
  memcpy(dup, s, n + 1);
  return dup;
}

static char *trim(char *s) {
  while (*s && isspace((unsigned char)*s)) s++;
  if (*s == '\0') return s;
  char *end = s + strlen(s) - 1;
  while (end > s && isspace((unsigned char)*end)) *end-- = '\0';
  return s;
}

static bool has_value(const char *s) {
  return s != NULL && *s != '\0';
}

static bool is_yes(const char *s) {
  return strcasecmp(s, "y") == 0 || strcasecmp(s, "yes") == 0 || strcmp(s, "1") == 0 ||
         strcasecmp(s, "true") == 0;
}

static bool is_no(const char *s) {
  return strcasecmp(s, "n") == 0 || strcasecmp(s, "no") == 0 || strcmp(s, "0") == 0 ||
         strcasecmp(s, "false") == 0;
}

static int prompt_line(const char *label, const char *default_value, bool allow_empty, char **out) {
  char buf[INPUT_BUFSZ];

  while (1) {
    if (default_value != NULL && *default_value) {
      fprintf(stderr, "%s [%s]: ", label, default_value);
    } else {
      fprintf(stderr, "%s: ", label);
    }

    if (fgets(buf, sizeof(buf), stdin) == NULL) {
      if (feof(stdin)) return 1;
      return -1;
    }

    char *value = trim(buf);
    if (*value == '\0') {
      if (default_value != NULL) {
        value = (char *)default_value;
      } else if (!allow_empty) {
        fprintf(stderr, "Value is required.\n");
        continue;
      }
    }

    if (!allow_empty && *value == '\0') {
      fprintf(stderr, "Value is required.\n");
      continue;
    }

    *out = xstrdup(value);
    return 0;
  }
}

static int prompt_hidden_line(const char *label, char **out) {
  char buf[INPUT_BUFSZ];
#ifdef _WIN32
  HANDLE hstdin = GetStdHandle(STD_INPUT_HANDLE);
  if (hstdin == INVALID_HANDLE_VALUE) return -1;

  DWORD mode = 0;
  if (!GetConsoleMode(hstdin, &mode)) return -1;

  DWORD hidden_mode = mode & ~ENABLE_ECHO_INPUT;
  if (!SetConsoleMode(hstdin, hidden_mode)) return -1;

  fprintf(stderr, "%s: ", label);
  if (fgets(buf, sizeof(buf), stdin) == NULL) {
    SetConsoleMode(hstdin, mode);
    if (feof(stdin)) return 1;
    return -1;
  }

  SetConsoleMode(hstdin, mode);
  fprintf(stderr, "\n");
#else
  if (!isatty(fileno(stdin))) {
    fprintf(stderr, "Hidden password entry requires a TTY.\n");
    return -1;
  }

  struct termios oldt, newt;
  if (tcgetattr(STDIN_FILENO, &oldt) != 0) return -1;

  newt = oldt;
  newt.c_lflag &= (tcflag_t)~ECHO;
  if (tcsetattr(STDIN_FILENO, TCSANOW, &newt) != 0) return -1;

  fprintf(stderr, "%s: ", label);
  if (fgets(buf, sizeof(buf), stdin) == NULL) {
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    if (feof(stdin)) return 1;
    return -1;
  }

  tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
  fprintf(stderr, "\n");
#endif

  char *value = trim(buf);
  *out = xstrdup(value);
  return 0;
}

static int prompt_yes_no(const char *label, bool default_value, bool *out) {
  char *input = NULL;
  int rc = 0;
  const char *default_text = default_value ? "y" : "n";

  while (1) {
    rc = prompt_line(label, default_text, false, &input);
    if (rc != 0) return rc;

    if (is_yes(input)) {
      *out = true;
      free(input);
      return 0;
    }
    if (is_no(input)) {
      *out = false;
      free(input);
      return 0;
    }

    free(input);
    input = NULL;
    fprintf(stderr, "Please answer yes or no.\n");
  }
}

static int prompt_int(const char *label, int default_value, int min, int max, int *out) {
  char default_buf[32];
  char *input = NULL;

  snprintf(default_buf, sizeof(default_buf), "%d", default_value);
  while (1) {
    int rc = prompt_line(label, default_buf, false, &input);
    if (rc != 0) return rc;

    char *endptr = NULL;
    errno = 0;
    long v = strtol(input, &endptr, 10);
    if (errno == 0 && endptr != input && *endptr == '\0' && v >= min && v <= max) {
      *out = (int)v;
      free(input);
      return 0;
    }

    free(input);
    input = NULL;
    fprintf(stderr, "Enter an integer between %d and %d.\n", min, max);
  }
}

static bool file_exists(const char *path) {
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

static bool contains_sep(const char *path) {
#ifdef _WIN32
  return strchr(path, '\\') != NULL || strchr(path, '/') != NULL;
#else
  return strchr(path, '/') != NULL;
#endif
}

static bool is_executable_file(const char *path) {
#ifdef _WIN32
  DWORD attrs = GetFileAttributesA(path);
  return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
  return ACCESS(path, X_OK) == 0;
#endif
}

static bool command_exists(const char *cmd) {
  if (!has_value(cmd)) return false;

  if (contains_sep(cmd)) return is_executable_file(cmd);

  const char *path = getenv("PATH");
  if (!has_value(path)) return false;

  char *path_copy = xstrdup(path);
  char *iter = path_copy;
  char *entry;

#ifdef _WIN32
  const char *extensions[] = {"", ".exe", ".cmd", ".bat", ".com"};
  while ((entry = strsep(&iter, ";")) != NULL) {
    if (!has_value(entry)) continue;
    for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
      char candidate[MAX_PATH];
      _snprintf(candidate, sizeof(candidate), "%s\\%s%s", entry, cmd, extensions[i]);
      candidate[sizeof(candidate) - 1] = '\0';
      if (is_executable_file(candidate)) {
        free(path_copy);
        return true;
      }
    }
  }
#else
  while ((entry = strsep(&iter, ":")) != NULL) {
    if (!has_value(entry)) continue;
    size_t n = strlen(entry) + strlen(cmd) + 2;
    char *candidate = xmalloc(n);
    snprintf(candidate, n, "%s/%s", entry, cmd);
    bool ok = is_executable_file(candidate);
    free(candidate);
    if (ok) {
      free(path_copy);
      return true;
    }
  }
#endif

  free(path_copy);
  return false;
}

static char *default_command(void) {
#ifdef _WIN32
  if (command_exists("powershell.exe")) return xstrdup("powershell.exe");
  if (command_exists("cmd.exe")) return xstrdup("cmd.exe");
  return xstrdup("cmd.exe");
#else
  const char *shell = getenv("SHELL");
  if (has_value(shell)) {
    const char *name = strrchr(shell, '/');
    name = name ? name + 1 : shell;
    if (command_exists(name)) return xstrdup(name);
  }
  const char *fallback[] = {"zsh", "bash", "sh"};
  for (size_t i = 0; i < sizeof(fallback) / sizeof(fallback[0]); i++) {
    if (command_exists(fallback[i])) return xstrdup(fallback[i]);
  }
  return xstrdup("sh");
#endif
}

static int arg_builder_add(struct arg_builder *ab, const char *value) {
  if (ab->len + 2 > ab->cap) {
    int next = ab->cap == 0 ? 16 : ab->cap * 2;
    ab->items = xrealloc(ab->items, sizeof(char *) * next);
    ab->cap = next;
  }
  ab->items[ab->len++] = xstrdup(value);
  ab->items[ab->len] = NULL;
  return 0;
}

static void arg_builder_free(struct arg_builder *ab) {
  if (ab == NULL || ab->items == NULL) return;
  for (int i = 0; i < ab->len; i++) free(ab->items[i]);
  free(ab->items);
  ab->items = NULL;
  ab->len = 0;
  ab->cap = 0;
}

static int split_command_line(const char *line, struct arg_builder *out) {
  char token[INPUT_BUFSZ];
  size_t tlen = 0;
  bool in_single = false;
  bool in_double = false;
  bool escape = false;

  for (const char *p = line;; p++) {
    char c = *p;
    bool end = c == '\0';

    if (!end && escape) {
      token[tlen++] = c;
      escape = false;
      continue;
    }

    if (!end && c == '\\' && !in_single) {
      escape = true;
      continue;
    }

    if (!end && c == '\'' && !in_double) {
      in_single = !in_single;
      continue;
    }

    if (!end && c == '"' && !in_single) {
      in_double = !in_double;
      continue;
    }

    if (end || (!in_single && !in_double && isspace((unsigned char)c))) {
      if (tlen > 0) {
        token[tlen] = '\0';
        arg_builder_add(out, token);
        tlen = 0;
      }
      if (end) break;
      continue;
    }

    if (tlen + 1 >= sizeof(token)) {
      fprintf(stderr, "Command is too long.\n");
      return -1;
    }

    token[tlen++] = c;
  }

  if (in_single || in_double || escape) {
    fprintf(stderr, "Unterminated quote or escape in command.\n");
    return -1;
  }

  return 0;
}

static char *normalize_base_path(char *path) {
  if (!has_value(path)) {
    free(path);
    return xstrdup("");
  }

  char *trimmed = trim(path);
  if (*trimmed == '\0') {
    free(path);
    return xstrdup("");
  }

  size_t n = strlen(trimmed);
  while (n > 1 && trimmed[n - 1] == '/') trimmed[--n] = '\0';

  if (trimmed[0] != '/') {
    char *prefixed = xmalloc(n + 2);
    prefixed[0] = '/';
    memcpy(prefixed + 1, trimmed, n + 1);
    free(path);
    return prefixed;
  }

  char *normalized = xstrdup(trimmed);
  free(path);
  return normalized;
}

static char *build_credential(const char *username, const char *password) {
  size_t n = strlen(username) + strlen(password) + 2;
  char *credential = xmalloc(n);
  snprintf(credential, n, "%s:%s", username, password);
  return credential;
}

static bool is_shell_safe_char(char c) {
  return isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.' || c == '/' || c == ':';
}

static char *quote_shell_arg(const char *arg) {
#ifdef _WIN32
  return xstrdup(quote_arg(arg));
#else
  bool safe = has_value(arg);
  for (const char *p = arg; *p; p++) {
    if (!is_shell_safe_char(*p)) {
      safe = false;
      break;
    }
  }
  if (safe) return xstrdup(arg);

  size_t extra = 2;
  for (const char *p = arg; *p; p++) {
    if (*p == '\'') extra += 3;
    extra++;
  }

  char *out = xmalloc(extra + 1);
  char *w = out;
  *w++ = '\'';
  for (const char *p = arg; *p; p++) {
    if (*p == '\'') {
      memcpy(w, "'\\''", 4);
      w += 4;
    } else {
      *w++ = *p;
    }
  }
  *w++ = '\'';
  *w = '\0';
  return out;
#endif
}

static char *render_command(char **argv, int argc) {
  size_t total = 0;
  char **quoted = xmalloc(sizeof(char *) * argc);
  for (int i = 0; i < argc; i++) {
    quoted[i] = quote_shell_arg(argv[i]);
    total += strlen(quoted[i]) + 1;
  }

  char *rendered = xmalloc(total + 1);
  rendered[0] = '\0';
  for (int i = 0; i < argc; i++) {
    strcat(rendered, quoted[i]);
    if (i + 1 < argc) strcat(rendered, " ");
    free(quoted[i]);
  }
  free(quoted);
  return rendered;
}

static int build_argv(const char *program_name, struct wizard_config *cfg, struct wizard_output *out) {
  struct arg_builder cmd = {0};
  struct arg_builder args = {0};

  if (split_command_line(cfg->command_line, &cmd) != 0) {
    arg_builder_free(&cmd);
    return -1;
  }

  if (cmd.len == 0) {
    fprintf(stderr, "Command is required.\n");
    arg_builder_free(&cmd);
    return -1;
  }

  arg_builder_add(&args, program_name);
  arg_builder_add(&args, "-i");
  arg_builder_add(&args, cfg->interface);

  char port[32];
  snprintf(port, sizeof(port), "%d", cfg->port);
  arg_builder_add(&args, "-p");
  arg_builder_add(&args, port);

  if (has_value(cfg->base_path)) {
    arg_builder_add(&args, "-b");
    arg_builder_add(&args, cfg->base_path);
  }

  if (cfg->writable) arg_builder_add(&args, "-W");

  if (cfg->use_auth) {
    char *credential = build_credential(cfg->username, cfg->password);
    arg_builder_add(&args, "-c");
    arg_builder_add(&args, credential);
    free(credential);
  }

  if (cfg->use_tls) {
    arg_builder_add(&args, "-S");
    arg_builder_add(&args, "-C");
    arg_builder_add(&args, cfg->cert_path);
    arg_builder_add(&args, "-K");
    arg_builder_add(&args, cfg->key_path);
    if (has_value(cfg->ca_path)) {
      arg_builder_add(&args, "-A");
      arg_builder_add(&args, cfg->ca_path);
    }
  }

  if (cfg->max_clients > 0) {
    char max_clients[32];
    snprintf(max_clients, sizeof(max_clients), "%d", cfg->max_clients);
    arg_builder_add(&args, "-m");
    arg_builder_add(&args, max_clients);
  }

  if (cfg->once) arg_builder_add(&args, "-o");
  if (cfg->exit_no_conn) arg_builder_add(&args, "-q");
  if (cfg->open_browser) arg_builder_add(&args, "-B");

  for (int i = 0; i < cmd.len; i++) arg_builder_add(&args, cmd.items[i]);

  out->argc = args.len;
  out->argv = args.items;
  out->command_preview = render_command(out->argv, out->argc);

  arg_builder_free(&cmd);
  return 0;
}

static int prompt_existing_file(const char *label, char **out, bool allow_empty) {
  while (1) {
    char *value = NULL;
    int rc = prompt_line(label, "", allow_empty, &value);
    if (rc != 0) return rc;

    if (!has_value(value)) {
      *out = value;
      return 0;
    }

    if (file_exists(value)) {
      *out = value;
      return 0;
    }

    free(value);
    fprintf(stderr, "File does not exist: try again.\n");
  }
}

static int prompt_command_line(char **out) {
  char *default_cmd = default_command();
  int rc = 0;

  while (1) {
    rc = prompt_line("Command to run", default_cmd, false, out);
    if (rc != 0) {
      free(default_cmd);
      return rc;
    }

    struct arg_builder cmd = {0};
    if (split_command_line(*out, &cmd) != 0 || cmd.len == 0) {
      free(*out);
      *out = NULL;
      arg_builder_free(&cmd);
      fprintf(stderr, "Please enter a valid command.\n");
      continue;
    }

    if (!command_exists(cmd.items[0])) {
      fprintf(stderr, "Command not found in PATH: %s\n", cmd.items[0]);
      free(*out);
      *out = NULL;
      arg_builder_free(&cmd);
      continue;
    }

    arg_builder_free(&cmd);
    free(default_cmd);
    return 0;
  }
}

static int prompt_final_action(enum wizard_action *action) {
  char *choice = NULL;
  while (1) {
    int rc = prompt_line("Select action: [1] Run now [2] Print command only [3] Cancel", "1", false, &choice);
    if (rc != 0) return rc;

    if (strcmp(choice, "1") == 0) {
      *action = WIZARD_ACTION_RUN;
      free(choice);
      return 0;
    }
    if (strcmp(choice, "2") == 0) {
      *action = WIZARD_ACTION_PRINT;
      free(choice);
      return 0;
    }
    if (strcmp(choice, "3") == 0) {
      *action = WIZARD_ACTION_CANCEL;
      free(choice);
      return 0;
    }

    free(choice);
    choice = NULL;
    fprintf(stderr, "Please select 1, 2, or 3.\n");
  }
}

int wizard_interactive(const char *program_name, struct wizard_output *output) {
  struct wizard_config cfg;
  memset(&cfg, 0, sizeof(cfg));
  memset(output, 0, sizeof(*output));

  cfg.port = 7681;
  cfg.max_clients = 0;

  fprintf(stderr, "ttyd interactive setup\n");
  fprintf(stderr, "Press Enter to accept defaults.\n\n");

  int rc = prompt_command_line(&cfg.command_line);
  if (rc != 0) goto done;

  rc = prompt_line("Interface", "127.0.0.1", false, &cfg.interface);
  if (rc != 0) goto done;

  rc = prompt_int("Port", 7681, 0, 65535, &cfg.port);
  if (rc != 0) goto done;

  rc = prompt_line("Base path (optional)", "", true, &cfg.base_path);
  if (rc != 0) goto done;
  cfg.base_path = normalize_base_path(cfg.base_path);

  rc = prompt_yes_no("Allow terminal input (writable)? [y/n]", false, &cfg.writable);
  if (rc != 0) goto done;

  rc = prompt_yes_no("Enable basic auth? [y/n]", false, &cfg.use_auth);
  if (rc != 0) goto done;
  if (cfg.use_auth) {
    while (1) {
      rc = prompt_line("Basic auth username", NULL, false, &cfg.username);
      if (rc != 0) goto done;
      if (strchr(cfg.username, ':') != NULL) {
        fprintf(stderr, "Username cannot contain ':'.\n");
        free(cfg.username);
        cfg.username = NULL;
        continue;
      }
      break;
    }

    while (1) {
      char *confirm = NULL;
      rc = prompt_hidden_line("Basic auth password", &cfg.password);
      if (rc != 0) goto done;
      rc = prompt_hidden_line("Confirm password", &confirm);
      if (rc != 0) {
        free(confirm);
        goto done;
      }
      if (strcmp(cfg.password, confirm) == 0) {
        free(confirm);
        break;
      }
      fprintf(stderr, "Passwords do not match, try again.\n");
      free(cfg.password);
      cfg.password = NULL;
      free(confirm);
    }
  }

  rc = prompt_yes_no("Enable TLS (HTTPS)? [y/n]", false, &cfg.use_tls);
  if (rc != 0) goto done;

  if (cfg.use_tls) {
#if defined(LWS_OPENSSL_SUPPORT) || defined(LWS_WITH_TLS)
    rc = prompt_existing_file("TLS certificate path", &cfg.cert_path, false);
    if (rc != 0) goto done;
    rc = prompt_existing_file("TLS private key path", &cfg.key_path, false);
    if (rc != 0) goto done;
    rc = prompt_existing_file("TLS CA path (optional)", &cfg.ca_path, true);
    if (rc != 0) goto done;
#else
    fprintf(stderr, "TLS is not supported by this build.\n");
    rc = -1;
    goto done;
#endif
  }

  rc = prompt_int("Max clients (0 = unlimited)", 0, 0, INT_MAX, &cfg.max_clients);
  if (rc != 0) goto done;

  rc = prompt_yes_no("Exit when first client disconnects? [y/n]", false, &cfg.once);
  if (rc != 0) goto done;

  rc = prompt_yes_no("Exit when all clients disconnect? [y/n]", false, &cfg.exit_no_conn);
  if (rc != 0) goto done;

  rc = prompt_yes_no("Open browser after start? [y/n]", false, &cfg.open_browser);
  if (rc != 0) goto done;

  if (build_argv(program_name, &cfg, output) != 0) {
    rc = -1;
    goto done;
  }

  fprintf(stderr, "\nResolved command:\n%s\n\n", output->command_preview);

  rc = prompt_final_action(&output->action);
  if (rc != 0) goto done;

  if (output->action == WIZARD_ACTION_CANCEL) {
    wizard_output_free(output);
  }

done:
  wizard_config_free(&cfg);
  if (rc == 1) {
    fprintf(stderr, "\nWizard cancelled.\n");
    output->action = WIZARD_ACTION_CANCEL;
    return 0;
  }
  if (rc != 0) {
    wizard_output_free(output);
    return -1;
  }
  return 0;
}

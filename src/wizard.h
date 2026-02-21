#ifndef TTYD_WIZARD_H
#define TTYD_WIZARD_H

#include <stdbool.h>

enum wizard_action {
  WIZARD_ACTION_RUN = 1,
  WIZARD_ACTION_PRINT = 2,
  WIZARD_ACTION_CANCEL = 3,
};

struct wizard_output {
  enum wizard_action action;
  int argc;
  char **argv;
  char *command_preview;
};

int wizard_interactive(const char *program_name, struct wizard_output *output);
void wizard_output_free(struct wizard_output *output);

#endif

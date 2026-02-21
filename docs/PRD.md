# PRD: Interactive `ttyd` Configuration Wizard

## Problem Statement
`ttyd` is currently configured through command-line flags and a command payload (for example: `ttyd -p 7681 -W bash`).
For many users, especially first-time users, discovering valid option combinations and producing a runnable command is error-prone.

We need an interactive configuration experience when users type `ttyd` directly in a shell, while preserving existing non-interactive CLI behavior.

## Goals
1. Provide an interactive configuration wizard when `ttyd` is invoked without a command payload.
2. Reuse and reflect existing `ttyd` configuration semantics and defaults.
3. Generate and run a valid `ttyd` server command at the end of the interaction.
4. Ensure the final flow works on user local machines regardless of OS or CPU architecture (os/architecture-agnostic behavior).

## Non-Goals
- Replacing or removing existing flag-based usage.
- Introducing remote/cloud deployment workflow.
- Adding GUI dependencies.

## Current Configuration Method (Baseline)
`ttyd` today is configured via CLI options parsed in `src/server.c` (`getopt_long`) plus a target command.
Examples of existing options include:
- Network/bind: `-p/--port`, `-i/--interface`, `-6/--ipv6`, `-b/--base-path`
- Auth/security: `-c/--credential`, `-H/--auth-header`, `-O/--check-origin`, TLS flags (`-S`, `-C`, `-K`, `-A`)
- Session behavior: `-W/--writable`, `-m/--max-clients`, `-o/--once`, `-q/--exit-no-conn`
- Client prefs passthrough: `-t/--client-option`
- Process execution: `<command> [args...]`, `-w/--cwd`, `-u/--uid`, `-g/--gid`, `-s/--signal`

When called with no arguments, current behavior is help text output.

## Proposed User Experience
### Entry Trigger
- If user runs `ttyd` with no command payload, offer interactive mode.
- Keep existing behavior for explicit flags and command usage (for backwards compatibility).

### Wizard Flow
1. Detect local environment details (OS, architecture, shell, candidate default command).
2. Ask for target command to expose (default based on platform; examples: `bash`, `zsh`, `sh`, `cmd`, `powershell`).
3. Ask networking choices:
- listen interface (`127.0.0.1` default)
- port (`7681` default)
- optional base path
4. Ask access/security choices:
- writable vs readonly
- optional basic auth
- optional TLS (if cert/key available)
5. Ask runtime behavior:
- max clients / once / exit on disconnect
- optional open browser after start
6. Show final resolved `ttyd` command preview.
7. Final confirmation:
- `Run now`
- `Print command only`
- `Cancel`

### Final Interaction Requirement
At the final interaction, user must be able to run a working local `ttyd` server immediately from the wizard output.

## Functional Requirements
1. Wizard must map every prompted field to existing supported CLI flags.
2. Wizard must never require features unavailable on the current platform.
3. Wizard must validate required values before launch (port range, auth format, file existence for TLS paths).
4. Wizard must produce a deterministic command preview matching selected options.
5. Wizard must allow non-interactive invocation to remain unchanged.

## Platform / Portability Requirements
1. Implementation must be OS-agnostic and architecture-agnostic:
- Linux/macOS/Windows supported in behavior.
- x86_64/arm64 and other supported targets should require no architecture-specific wizard logic.
2. Command defaults should adapt per OS but all prompts/validation remain uniform.
3. No platform-specific UI frameworks; shell/TTY interaction only.

## UX and Error Handling Requirements
1. Clear defaults shown for each step.
2. Users can accept defaults quickly (minimal keystrokes).
3. Invalid input must re-prompt with actionable error text.
4. If selected command is unavailable, provide fallback suggestions and retry.
5. Cancellation exits cleanly without side effects.

## Backward Compatibility
1. Existing documented CLI options continue to work exactly as-is.
2. Existing scripts using `ttyd` must not break.
3. `--help` and `--version` behavior remains unchanged.

## Implementation Notes (Design-Level)
1. Build wizard as a thin layer that outputs standard argv/options consumed by current parser path.
2. Prefer centralized option model to avoid divergence between wizard and manual CLI behavior.
3. Keep feature flags and defaults sourced from existing runtime structures where possible.

## Acceptance Criteria
1. Typing `ttyd` in a shell enters interactive configuration mode.
2. Completing wizard with defaults starts a local ttyd session successfully.
3. Completing wizard with custom values yields a command preview identical to launch arguments.
4. Same binary behavior is valid across supported OS and architectures without code forks in wizard logic.
5. Existing manual usage (`ttyd [options] <command>`) remains fully functional.

## Open Questions
1. Should no-arg invocation always launch wizard, or require explicit opt-in (for example environment variable / build option) to preserve legacy help-text expectation?
2. Should wizard persist reusable profiles (future scope) or remain stateless in v1?
3. Should credential input support hidden password entry in v1?

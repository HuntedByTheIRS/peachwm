# Contributing

PeachWM is a completely open source project,
and grateful for any outside contributions that improve the project.
If you'd like to contribute, fork the project and create a pull request or issue.

Before contributing, please consider the following guidelines.

## How to Contribute

1. **Discuss first** -- Open an issue to discuss significant changes before
   implementing them. This avoids wasted effort if the change isn't a good fit.
2. **Keep it focused** -- Each pull request should address a single concern.
   Avoid mixing bug fixes, refactors, and features in the same PR.
3. **Test your changes** -- Run `make debug` (Clang with `-Weverything` and
   sanitizers) and `make release` (`-Werror`) to ensure no warnings or errors.
4. **Match the existing style** -- See the Coding Style section below.

## Coding Style

PeachWM follows a consistent style throughout the codebase. Please match it.

### Indentation and Formatting

- **2-space indentation** -- No tabs, no 4-space indents.
- **K&R brace style** -- Opening brace on the same line as the statement.
- **Return type on its own line** -- Every function declaration and definition
  places the return type on a separate line:

  ```c
  static inline int
  client_is_x11(Client *c)
  {
    ...
  }
  ```

- **No extra spaces inside parentheses** -- `if (cond)`, `func(a, b)`.
- **Pointer asterisk attached to the name** -- `Client *c`, not `Client* c`.

### Naming Conventions

- **Functions and variables:** `snake_case`
- **Types (structs, enums, typedefs):** `PascalCase`
- **Macros:** `UPPER_SNAKE_CASE`
- **Enum values:** `PascalCase` (e.g., `CurNormal`, `XDGShell`, `LyrBg`)

### Code Patterns

- **Use `wl_list`** for intrusive linked lists (Wayland native pattern).
- **Use the `LISTEN` macro** to attach `wl_signal` handlers:

  ```c
  LISTEN(&server->new_output, &output_listener, handle_new_output);
  ```

- **Follow the `wl_container_of` pattern** in event listeners:

  ```c
  void
  handle_new_output(struct wl_listener *listener, void *data)
  {
    Monitor *m = wl_container_of(listener, m, frame);
    ...
  }
  ```

- **Guard X11-specific code** with `#ifdef XWAYLAND` / `#endif`.
- **Use `ecalloc()`** instead of raw `malloc()` -- it calls `die()` on OOM.
- **Prefer static inline functions in headers** over function pointers,
  consistent with `include/client.h`.

### Build and Compiler

- The project uses **C23** (`-std=c23`).
- The **Makefile** uses GNU make extensions (`.ONESHELL`, `ifeq`, target-specific
  variables). Do not introduce autotools, CMake, or meson.
- Ensure your code compiles cleanly with `make debug` and `make release` -- the
  project requires Clang (default) and the LLD linker for building. CI checks
  both targets.

These guidelines are suggestions, not hard rules. When in doubt, match the
style of the surrounding code.

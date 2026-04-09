# Project Guidelines

## Code Quality

- Before writing code, consider appropriate design patterns (Strategy, Factory, RAII, etc.) and overall structure.
- During bug fixes or improvements, keep the surrounding code clean — do not let incremental changes accumulate into messy code.
- Prefer clear separation of concerns: transport, platform, service, and session layers must remain independent.
- Avoid mixing responsibilities within a single class.

## Language

- All source code comments, log messages, and string literals must be in English only.
- No Korean, no emojis — anywhere in source files.

## Style

- Follow the existing code style in the repository (C++17, 4-space indent, braces on same line for functions).
- Log tags use the format: `#define LOG_TAG "ClassName"`.
- Use `AA_LOG_I()`, `AA_LOG_D()`, `AA_LOG_W()`, `AA_LOG_E()` for logging (defined in Logger.hpp).

## Plans

- All plans for this project are stored in this repository under `docs/plans/`.
- Plan filenames are prefixed with a four-digit zero-padded number (e.g. `0001_intro.md`, `0002_audio_focus.md`).
- When creating a new plan file, scan the existing files in `docs/plans/` and assign the next sequential number — never reuse a number.
- Do not store plans for this project in the user-global `~/.claude/plans/` directory; that location is only Claude Code's temporary plan-mode workspace and is not project-scoped.

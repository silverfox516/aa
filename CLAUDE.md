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

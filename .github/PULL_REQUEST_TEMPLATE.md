## Summary

<!-- One or two sentences describing what this PR does and why. -->

## Type of change

- [ ] Bug fix (non-breaking — fixes an issue)
- [ ] New feature (non-breaking — adds functionality)
- [ ] Breaking change (fix or feature that changes existing behaviour)
- [ ] Refactor (no functional change)
- [ ] Documentation / comments only
- [ ] CI / build system

## Related issues

<!-- Closes #NNN  /  Fixes #NNN  /  Related to #NNN -->

## Changes

<!-- Bullet-point list of what changed. Be specific about files/functions. -->

-
-

## How to test

<!-- Steps to reproduce / verify the fix or feature manually. -->

1.
2.

## Checklist

- [ ] CI passes (lint + build + tests)
- [ ] New code follows `snake_case` (C) / `PascalCase` (Casprix) conventions
- [ ] No `printf` in runtime hot paths — use `CPX_LOG_*` macros
- [ ] New public C functions have declarations in the appropriate `include/casprix/*.h`
- [ ] New `.cpx` library modules are in `lib/` (not in `include/`)
- [ ] `docs/PROJECT_STRUCTURE.md` updated if directories were added/moved
- [ ] `CHANGELOG.md` entry added under `[Unreleased]`

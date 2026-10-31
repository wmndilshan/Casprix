# Security policy

## Supported versions

Casprix is pre-stable software. Security fixes are applied to the current
`main` branch and the latest tagged release only. No long-term-support branch
is currently maintained.

## Reporting a vulnerability

Do not open a public issue for a suspected vulnerability. Use GitHub's private
security advisory feature for this repository. Include:

- the affected component and version;
- a minimal reproducer or proof of concept;
- expected impact;
- relevant platform and build configuration; and
- any suggested mitigation.

Maintainers should acknowledge a report within seven days. Disclosure timing is
coordinated after a fix or mitigation is available.

## Scope

Compiler crashes, generated-code memory unsafety, package-manager path traversal,
malicious package handling, and runtime memory-corruption issues are in scope.
The experimental JIT, network server, Android toolchain, and AI model loaders
should not be exposed to untrusted inputs without additional isolation.

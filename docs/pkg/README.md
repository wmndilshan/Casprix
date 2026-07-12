# Casprix package manager (`casprix-pkg`)

Package management for the Casprix toolchain. The executable produced by CMake is **`casprix-pkg`**; the in-app usage text refers to the command as **`cpkg`**.

## Features

- **Dependency resolution** — automatic resolution with semantic versioning
- **Version constraints** — support for `^`, `~`, `>=`, wildcards
- **Local cache** — fast installs with a local package cache
- **Package registry** — publish and share packages (when configured)
- **Manifest format** — `casper.json`
- **Cross-platform** — Windows, Linux, macOS

## Building

From the **repository root** (recommended; enables the same flags as the main project):

```bash
cmake -S . -B build -DBUILD_PACKAGE_MANAGER=ON
cmake --build build --target casprix-pkg
```

The binary is written to the build directory (e.g. `build/casprix-pkg` or `build/Release/casprix-pkg.exe` on Windows multi-config generators).

**Standalone** build of only the package manager:

```bash
cmake -S pkg -B build-pkg -DCASPRIX_PKG_STANDALONE=ON
cmake --build build-pkg
```

## Quick start

### 1. Create a new package

```bash
casprix-pkg init my-awesome-package
cd my-awesome-package
```

This creates:

```
my-awesome-package/
├── casper.json
├── src/
│   └── main.cpx
└── README.md
```

### 2. Install dependencies

```bash
# Install all dependencies from casper.json
casprix-pkg install

# Install specific package
casprix-pkg install http-client

# Install specific version
casprix-pkg install json-parser@1.5.3
```

### 3. Publish your package

```bash
casprix-pkg login
casprix-pkg publish
```

## Commands

### Package management

| Command | Description |
|---------|-------------|
| `casprix-pkg init [name]` | Initialize new package |
| `casprix-pkg install [package]` | Install package(s) |
| `casprix-pkg update` | Update all dependencies |
| `casprix-pkg remove <package>` | Remove package |
| `casprix-pkg list` | List installed packages |
| `casprix-pkg search <query>` | Search registry |
| `casprix-pkg info <package>` | Show package details |

### Publishing

| Command | Description |
|---------|-------------|
| `casprix-pkg login` | Login to registry |
| `casprix-pkg logout` | Logout |
| `casprix-pkg pack` | Create package tarball |
| `casprix-pkg publish` | Publish to registry |
| `casprix-pkg unpublish <ver>` | Unpublish version |

### Cache

| Command | Description |
|---------|-------------|
| `casprix-pkg cache list` | List cached packages |
| `casprix-pkg cache clean` | Clear cache |

## `casper.json` format

```json
{
  "name": "my-package",
  "version": "1.0.0",
  "description": "My awesome package",
  "author": "Your Name",
  "license": "MIT",
  "main": "src/main.cpx",
  "repository": "https://github.com/wmndilshan/casprix",
  "dependencies": {
    "http-client": "^2.1.0",
    "json-parser": "~1.5.3"
  },
  "devDependencies": {
    "test-framework": "^3.0.0"
  }
}
```

## Version constraints

| Constraint | Meaning | Example |
|------------|---------|---------|
| `1.2.3` | Exact version | `1.2.3` only |
| `^1.2.3` | Compatible | `1.2.3` ≤ x < `2.0.0` |
| `~1.2.3` | Approximately | `1.2.3` ≤ x < `1.3.0` |
| `>=1.2.3` | Greater or equal | `1.2.3` ≤ x |
| `>1.2.3` | Greater than | `1.2.3` < x |
| `1.2.x` | Wildcard | `1.2.0` ≤ x < `1.3.0` |
| `*` | Any version | Any |

## Directory layout

```
~/.cpkg/
├── cache/              # Package cache
│   ├── http-client/
│   │   └── 2.1.4/
│   └── json-parser/
│       └── 1.5.3/
└── auth.txt            # API key
```

## API (library)

Sources live under `pkg/core/`:

- **`semver.h/c`** — semantic versioning and constraint matching
- **`manifest.h/c`** — `casper.json` parsing
- **`cache.h/c`** — local package cache
- **`installer.h/c`** — package installation
- **`publisher.h/c`** — publishing
- **`registry.h/c`** — registry client
- **`resolver.h/c`** — dependency resolver

### Example (C)

```c
#include "installer.h"

pkg_install_package("http-client", "^2.0.0");
pkg_install_from_manifest("casper.json", false);
```

## License

MIT

## Contributing

See [CONTRIBUTING.md](../../CONTRIBUTING.md).

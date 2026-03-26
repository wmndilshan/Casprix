# Casperix Package Manager (cpkg)

Complete package management system for the Casperix programming language.

## Features

- **Dependency Resolution** - Automatic resolution with semantic versioning
- **Version Constraints** - Support for `^`, `~`, `>=`, wildcards
- **Local Cache** - Fast installations with local package cache
- **Package Registry** - Publish and share packages
- **Manifest Format** - Simple `casper.json` format
- **Cross-Platform** - Windows, Linux, macOS support

## Installation

```bash
# Build from source
cd pkg
gcc -o cpkg cli_complete.c manifest.c semver.c cache.c installer.c publisher.c registry.c resolver.c -I.
```

## Quick Start

### 1. Create a New Package

```bash
cpkg init my-awesome-package
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

### 2. Install Dependencies

```bash
# Install all dependencies from casper.json
cpkg install

# Install specific package
cpkg install http-client

# Install specific version
cpkg install json-parser@1.5.3
```

### 3. Publish Your Package

```bash
# Login to registry
cpkg login

# Publish
cpkg publish
```

## Commands

### Package Management

| Command | Description |
|---------|-------------|
| `cpkg init [name]` | Initialize new package |
| `cpkg install [package]` | Install package(s) |
| `cpkg update` | Update all dependencies |
| `cpkg remove <package>` | Remove package |
| `cpkg list` | List installed packages |
| `cpkg search <query>` | Search registry |
| `cpkg info <package>` | Show package details |

### Publishing

| Command | Description |
|---------|-------------|
| `cpkg login` | Login to registry |
| `cpkg logout` | Logout |
| `cpkg pack` | Create package tarball |
| `cpkg publish` | Publish to registry |
| `cpkg unpublish <ver>` | Unpublish version |

### Cache

| Command | Description |
|---------|-------------|
| `cpkg cache list` | List cached packages |
| `cpkg cache clean` | Clear cache |

## casper.json Format

```json
{
  "name": "my-package",
  "version": "1.0.0",
  "description": "My awesome package",
  "author": "Your Name",
  "license": "MIT",
  "main": "src/main.cpx",
  "repository": "https://github.com/user/repo",
  "dependencies": {
    "http-client": "^2.1.0",
    "json-parser": "~1.5.3"
  },
  "devDependencies": {
    "test-framework": "^3.0.0"
  }
}
```

## Version Constraints

| Constraint | Meaning | Example |
|------------|---------|---------|
| `1.2.3` | Exact version | `1.2.3` only |
| `^1.2.3` | Compatible | `1.2.3` ≤ x < `2.0.0` |
| `~1.2.3` | Approximately | `1.2.3` ≤ x < `1.3.0` |
| `>=1.2.3` | Greater or equal | `1.2.3` ≤ x |
| `>1.2.3` | Greater than | `1.2.3` < x |
| `1.2.x` | Wildcard | `1.2.0` ≤ x < `1.3.0` |
| `*` | Any version | Any |

## Directory Structure

```
~/.cpkg/
├── cache/              # Package cache
│   ├── http-client/
│   │   └── 2.1.4/
│   └── json-parser/
│       └── 1.5.3/
└── auth.txt            # API key
```

## API

### Components

- **`semver.h/c`** - Semantic versioning parser and constraint matching
- **`manifest.h/c`** - casper.json parser
- **`cache.h/c`** - Local package cache manager
- **`installer.h/c`** - Package installation
- **`publisher.h/c`** - Package publishing
- **`registry.h/c`** - Registry client
- **`resolver.h/c`** - Dependency resolver

### Example Usage

```c
#include "installer.h"

// Install package
pkg_install_package("http-client", "^2.0.0");

// Install from manifest
pkg_install_from_manifest("casper.json", false);
```

## Examples

### Create and Publish

```bash
# Create new package
cpkg init my-lib

# Add dependencies
cpkg install lodash@^4.0.0

# Test locally
casperix src/main.cpx

# Publish
cpkg login
cpkg publish
```

### Use in Project

```bash
# Install dependency
cpkg install my-lib

# Import in code
import "my-lib"
```

## Testing

```bash
# Run package manager tests
cd pkg
make test
```

## License

MIT

## Contributing

Contributions welcome! Please see [CONTRIBUTING.md](../CONTRIBUTING.md).

---

**Version**: 1.0.0  
**Status**: Production Ready  
**Language**: Casperix

# move-nested-files (mnf) 1.0.0

`mnf` verschiebt rekursiv **alle Dateien aus verschachtelten Unterverzeichnissen** eines Quellverzeichnisses in ein **einzelnes Zielverzeichnis**.

- Kollisionsmodi: `rename` (Default), `skip`, `overwrite`
- Threaded, progress output, Dry-Run
- Filter: `--include/--exclude` (Globs), `--allow-ext/--deny-ext`, `--min-size/--max-size`, `--newer-than/--older-than`
- Symlink-Unterstützung (optional), `--prune-empty-dirs`, Metadatenübernahme

## Build

```bash
make
# oder: gcc -O2 -pthread -Wall -Wextra -o mnf src/mnf.c
```

## Release (Linux)

```bash
make release
# erzeugt dist/mnf-1.0.0-linux-<arch>.tar.gz
```

## Installation

```bash
sudo make install PREFIX=/usr/local
# installiert nach /usr/local/bin/mnf und man-Seite nach /usr/local/share/man/man1/mnf.1.gz
```

## Nutzung

```bash
mnf SOURCE_DIR DEST_DIR [options]
mnf --help
```

### Beispiele

```bash
# Standard
mnf ./quelle ./ziel

# 4 Threads, nur Bilder >1MiB, Fortschritt
mnf ./quelle ./ziel --threads 4 --include "**/*.jpg,**/*.png" --min-size 1M --progress

# Trockenlauf, tmp-Ordner ausschließen
mnf ./quelle ./ziel --dry-run --exclude "**/tmp/**"
```

## Deinstallation

```bash
sudo make uninstall PREFIX=/usr/local
```

## Lizenz

Public Domain / Unlicense. Verwende es frei und ohne Gewähr.

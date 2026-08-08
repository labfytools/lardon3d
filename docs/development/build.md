# Instructions de build

## Prérequis

- **OS** : Linux (testé sur distributions récentes)
- **Compilateur** : Clang (recommandé) ou GCC
- **Système de build** : Meson + Ninja
- **Dépendances** : ncursesw (ncurses avec support Unicode)
- **Langage** : C17

## Installation des dépendances

```sh
# Debian / Ubuntu
sudo apt install clang meson ninja-build libncursesw5-dev pkg-config

# Fedora
sudo dnf install clang meson ninja-build ncurses-devel pkg-config

# Arch
sudo pacman -S clang meson ninja ncurses pkgconf
```

## Build standard

```sh
CC=clang meson setup build --wipe
meson compile -C build -j8
```

### Options utiles

```sh
# Build de debug (défaut)
meson setup build --wipe

# Build de release
meson setup build --wipe --buildtype=release

# Build avec optimisations aggressive
meson setup build --wipe --buildtype=release -Db_lto=true
```

## Validation

```sh
# Tests unitaires
meson test -C build --print-errorlogs

# Vérification du style (whitespace)
git diff --check
```

## Build ASan/UBSan (debug mémoire)

À exécuter pour tout ticket touchant la mémoire, les durées de vie ou les
allocations :

```sh
meson setup build-asan --wipe \
    -Db_sanitize=address,undefined \
    -Db_static=false
meson compile -C build-asan -j8
meson test -C build-asan --print-errorlogs
```

## Build TSan (concurrence)

À exécuter pour tout ticket touchant la concurrence (pthread, mutex,
variables de condition, états partagés) :

```sh
meson setup build-tsan --wipe \
    -Db_sanitize=thread \
    -Db_static=false
meson compile -C build-tsan -j8
meson test -C build-tsan --print-errorlogs
```

## Variables d'environnement

| Variable | Description |
|---|---|
| `CC` | Compilateur C (défaut : gcc) |
| `CFLAGS` | Drapeaux de compilation supplémentaires |
| `LDFLAGS` | Drapeaux de liaison supplémentaires |

## Structure du build

```text
build/
├── src/          # objets et binaires
├── tests/        # binaires de tests
└── compile_commands.json  # pour LSP / clangd
```

## Dépannage

### Erreur : ncursesw introuvable

```sh
# Vérifier l'installation
pkg-config --libs ncursesw
# Si absent, installer le paquet de développement ncursesw
```

### Erreur : clang introuvable

```sh
# Utiliser gcc en alternative
meson setup build --wipe
# ou installer clang
sudo apt install clang
```

### Build lent

```sh
# Réduire la parallélisation
meson compile -C build -j4
# ou utiliser ccache
CC="ccache clang" meson setup build --wipe
```

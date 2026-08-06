# Scan3D Vulkan

Scan3D Vulkan est une application terminal interactive dédiée à la reconstruction 3D. Cette première interface TUI utilise tout l'espace du terminal et s'adapte à son redimensionnement.

Une vue Vulkan séparée sera ajoutée ultérieurement pour afficher la scène 3D. L'application actuelle n'ouvre aucune fenêtre graphique.

## Prérequis

- Arch Linux (ou distribution dérivée)
- Clang
- Meson
- Ninja
- ncursesw

## Compilation

```sh
CC=clang meson setup build --wipe
meson compile -C build -j8
```

## Exécution

```sh
./build/scan3d-vulkan
```

La TUI s'affiche directement dans le terminal courant. Appuyez sur `q` ou `Q` pour quitter proprement.

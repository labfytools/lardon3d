# Lardon3D

Lardon3D est une application terminal interactive dédiée à la reconstruction 3D. Son interface TUI utilise tout l'espace du terminal et s'adapte à son redimensionnement.

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
./build/lardon3d
```

La TUI s'affiche directement dans le terminal courant. Appuyez sur `q` ou `Q` pour quitter proprement.

Les projets sont enregistrés par défaut dans `~/Documents/Lardon/Projets3D`.
La variable `LARDON3D_PROJECTS_ROOT` permet de choisir un autre répertoire racine à l'exécution.

L'import accepte les images JPEG, PNG, TIFF et HEIC présentes directement dans
le dossier choisi, sans parcourir ses sous-dossiers. Elles sont copiées vers
`images/originals` et répertoriées dans `images/manifest.tsv`.

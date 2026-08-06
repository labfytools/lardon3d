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
L'import s'exécute en arrière-plan afin que la TUI reste réactive. Pendant une
opération, la touche `C` demande son annulation.

L'écran Import présente un catalogue en mémoire vérifié par rapport au manifeste
et aux fichiers de `images/originals`. Les flèches, `j`/`k`, PageUp, PageDown,
Home et End naviguent dans la liste ; `R` recharge le catalogue.
La touche `S` fait défiler les tris par ordre d'import, nom ou taille, dans les
deux directions. `/` filtre les noms d'images sans distinction de casse ASCII et
`X` efface le filtre. Le tri et le filtre restent entièrement en mémoire et ne
modifient ni le manifeste ni les images.

Un moteur générique exécute les tâches en file FIFO sur un worker unique. Il
prend en charge progression, pause, reprise et annulation coopérative sans
dépendre de la TUI ni des modules métier. L'écran `F5` affiche l'état courant de
la file et de ses tâches.

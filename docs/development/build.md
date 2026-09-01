# Instructions de build

## Prérequis

- **OS** : Linux (testé sur distributions récentes)
- **Compilateur** : Clang (recommandé) ou GCC
- **Système de build** : Meson + Ninja
- **Dépendances principales** : ncursesw, SQLite, OpenSSL, GIO/GLib, OpenCV,
  LibRaw, libexif, libpng, libdeflate, Ceres ; Vulkan reste optionnel
- **Langages** : API publiques C17 et implémentation mixte C17/C++17

## Bootstrap des outils

Les commandes ci-dessous installent seulement le compilateur, Meson/Ninja,
`pkg-config` et ncurses. Les bibliothèques listées plus haut doivent aussi être
disponibles dans les versions acceptées par `meson.build`; Meson reste la source
de vérité et refuse explicitement une dépendance absente ou incompatible.

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
# Première configuration
CC=clang meson setup build

# Arbre existant
meson setup --reconfigure build
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

# Vérification autonome d'un header C public modifié
cc -x c -std=c17 -fsyntax-only -Iinclude \
  -include lardon3d/<header>.h /dev/null
```

### Preuve fraîche de maintenance globale — 1er septembre 2026

Le [registre canonique](../architecture/global_maintenance_audit.md) conserve
le détail et les qualifications. Les résultats reproductibles acquis sont :

| Configuration fraîche | Compilateurs/options | Build | Suite |
| --- | --- | ---: | ---: |
| portable | Clang/Clang++ 22.1.8, C17/C++17, `-Dvulkan_orb=disabled` | 931/931 | 64/64 sériel |
| Vulkan | Clang/Clang++ 22.1.8, C17/C++17, `-Dvulkan_orb=enabled` | 939/939 | 65/65 sériel |
| ASan/UBSan portable | Clang/Clang++ 22.1.8, `address,undefined` | graphe complet | 64/64 avec LSan désactivé après attribution externe |
| TSan portable | GCC/G++ 16.2.1, Vulkan désactivé | cibles concurrentes | 14/14 + 220 répétitions |

La suite Vulkan comprend `orb-vulkan-backend` sur la Radeon 780M RADV PHOENIX
réelle. La cible de feasibility SIFT/RootSIFT, non enregistrée dans la suite,
a été compilée/exécutée séparément : zéro divergence de décision Lowe mais des
divergences d'index et de bits de distance, donc aucune promotion en backend
production. Les probes stricts GCC/Clang C17+C++17 passent 76/76 sur les
19 headers publics modifiés ou nouveaux, ainsi que le fixture ABI, le lien
application et `git diff --check`; `scan3d/` reste intact.

L'unique revue finale indépendante GPT-5.6 SOL/ULTRA a conclu PASS sans finding
bloquant. Elle a indépendamment rejoué le build portable, la suite complète
64/64, une matrice focalisée 15/15, les 76/76 probes de headers, l'ABI, les
négatifs de seams production, le SHA-256 du manifest GV retenu et le diff-check.
Le statut canonique est donc `GLOBAL_MAINTENANCE_AUDIT=PASS/FROZEN` ; les
qualifications sanitizer ci-dessous restent néanmoins partie de la preuve.

Une validation post-freeze a ensuite attribué le délai intermittent de
`test-feature-task` à la capture de la télémétrie hôte réelle par ses Governors
synthétiques. Le fixture utilise maintenant un `ResourceSnapshot` complet,
privé, par Governor et compilé pour cette seule cible ; production continue de
lire la télémétrie réelle et n'exporte aucun seam. Après correction du second
Governor relevé en revue, Feature passe 100/100, la matrice ordonnée 4/4 et les
suites finales portable/Vulkan 64/64 et 65/65 ; ASan/UBSan ciblé avec
`detect_leaks=0` et TSan passent. Le registre canonique conserve la régression
charge 5 `WAIT`/charge 0 `START` et la qualification exacte. Un timeout `task`
isolé dans une suite normale mixte après reconstruction large reste
non reproductible : le ciblé immédiat et sa matrice de revue 100/100 passent,
sans modification de Task ni de son timeout.

La première suite LSan complète est volontairement conservée comme non-PASS :
57 OK, 6 FAIL et 1 timeout. Cinq échecs partagent exactement la fuite externe
OpenCL de 3 808 octets/68 allocations ; les deux anomalies de 30 s n'ont aucun
diagnostic sanitizer. Le délai Feature, absent du suivi initial, a ensuite été
reproduit et corrigé comme décrit ci-dessus ; le délai Task reste non
reproductible. La suite entière passe 64/64 avec ASan/UBSan actifs et
`detect_leaks=0`, tandis qu'un sous-ensemble prouvé sans loader OpenCV/OpenCL
passe 20/20 avec LSan actif. Il est donc incorrect de résumer cette preuve par
« LSan 64/64 ».

Le log Clang complet a aussi été audité. La conversion publique Sparse SfM
`uint32_t → int` était matérielle et a été corrigée avec validation ciblée ; les
autres émissions sont soit des conversions baseline déjà bornées, soit des
tests/benchmarks, soit des headers OpenCV/Ceres externes. Les emplacements et
justifications exacts restent centralisés dans le registre afin de ne pas
dupliquer une seconde liste normative ici.

## Build ASan/UBSan (debug mémoire)

À exécuter pour tout ticket touchant la mémoire, les durées de vie ou les
allocations :

```sh
CC=clang meson setup build-asan --wipe \
    -Db_sanitize=address,undefined
meson compile -C build-asan -j8
meson test -C build-asan --print-errorlogs
```

## Build TSan (concurrence)

À exécuter pour tout ticket touchant la concurrence (pthread, mutex,
variables de condition, états partagés) :

```sh
CC=clang meson setup build-tsan --wipe \
    -Db_sanitize=thread \
    -Db_lundef=false
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

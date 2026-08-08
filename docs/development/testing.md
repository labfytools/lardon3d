# Procédures de test

## Vue d'ensemble

Lardon3D utilise le framework de test intégré à Meson. Chaque module possède
un fichier de test dans `tests/` correspondant au module testé.

## Lancer les tests

```sh
# Tous les tests
meson test -C build --print-errorlogs

# Un test spécifique
meson test -C build test_task_queue --print-errorlogs

# Tests avec verbose
meson test -C build -v --print-errorlogs

# Réexécuter uniquement les tests échoués
meson test -C build --reprint=failed
```

## Structure des tests

```text
tests/
├── test_task_queue.c    # tests de la file de tâches
├── test_task.c          # tests du module task
├── test_resource_governor.c  # tests du gouverneur
├── test_hardware_profile.c   # tests du profil matériel
├── test_import.c        # tests de l'import
├── test_project.c       # tests des projets
└── test_*.c             # autres modules
```

## Écrire un test

```c
#include <glib.h>
#include "lardon3d/task.h"

void test_task_create(void) {
    task_estimate_t est = {
        .ram_bytes = 1024 * 1024,
        .gpu_bytes = 0,
        .cpu_weight = 1,
        .io_weight = 0,
        .batch_size = 10,
        .batch_max = 100
    };
    task_t *t = task_create("test", &est, NULL, NULL);
    g_assert_nonnull(t);
    g_assert_cmpint(task_get_state(t), ==, TASK_STATE_IDLE);
    task_destroy(t);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/task/create", test_task_create);
    return g_test_run();
}
```

## Conventions

1. **Préfixe `test_`** : chaque fonction de test porte le préfixe `test_`.
2. **Chemin hiérarchique** : le nom du test suit le pattern `/module/action`.
3. **Asserts GLib** : utiliser `g_assert_*` pour les vérifications.
4. **Nettoyage** : chaque test libère toutes ses ressources.
5. **Isolation** : un test ne dépend pas de l'état d'un autre test.
6. **Déterminisme** : les tests ne dépendent pas de l'heure, du filesystem
   ou de l'état réseau (sauf test d'import).

## Tests unitaires vs tests d'intégration

`test-visual-index` couvre les descriptors synthétiques, le retrieval ORB réel,
les filtres inter-ScanSets, quatre queries concurrentes, la corruption/absence/
troncature d'un segment et 4 000 Feature Sets synthétiques. Le scénario de
reprise `visual_index.update` est exercé dans `test-feature-task`.

| Type | Portée | Fichier |
|---|---|---|
| Unitaire | Un module isolé | `tests/test_<module>.c` |
| Intégration | Interaction entre modules | `tests/test_<module>.c` avec dépendances réelles |

## Validation par ticket

Avant de livrer un ticket, exécuter la séquence complète :

```sh
# 1. Build clean
CC=clang meson setup build --wipe
meson compile -C build -j8

# 2. Tests
meson test -C build --print-errorlogs

# 3. Style
git diff --check

# 4. Si mémoire/concurrence touchés
CC=clang meson setup build-asan --wipe -Db_sanitize=address,undefined
meson compile -C build-asan -j8
meson test -C build-asan --print-errorlogs

# 5. Si concurrence touchée
CC=clang meson setup build-tsan --wipe -Db_sanitize=thread -Db_lundef=false
meson compile -C build-tsan -j8
meson test -C build-tsan --print-errorlogs
```

## Dépannage

### Test qui échoue en ASan

Vérifier les durées de vie des allocations. Ne jamais libérer un objet puis
y accéder. Vérifier que chaque `task_destroy()` est appelée.

### Test qui échoue en TSan

Vérifier que toutes les variables partagées sont protégées par un mutex.
Vérifier que ncurses est utilisé uniquement depuis le thread principal.

### Test qui échoue uniquement en release

Vérifier les assertions et les overflow arithmétiques. Compiler avec
`-fsanitize=undefined` pour détecter les comportements indéfinis.

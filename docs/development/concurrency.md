# Règles de concurrence

## Vue d'ensemble

Lardon3D utilise un modèle de concurrence à thread unique pour ncurses
et un modèle multi-thread pour le traitement. La séparation est stricte :
le thread ncurses ne fait jamais de travail métier, et les workers ne
touchent jamais ncurses.

## Modèle de concurrence

```text
Thread principal (ncurses)
├── Gestion des entrées
├── Affichage TUI
└── Orchestration

Worker thread
├── Exécution des tâches
├── Calculs métier
└── Écritures de résultats
```

## Règles fondamentales

### 1. ncurses appartient au thread principal

```c
// ✅ Correct : appel depuis le thread principal
mvprintw(0, 0, "Progression: %d%%", progress);

// ❌ Interdit : appel depuis un worker
// mvprintw() dans un thread secondaire
```

### 2. Variables partagées protégées par mutex

```c
// ✅ Correct
pthread_mutex_lock(&queue->mutex);
queue->count++;
pthread_mutex_unlock(&queue->mutex);

// ❌ Interdit
// queue->count++; sans protection
```

### 3. Variables de condition pour la synchronisation

```c
// Producteur (scheduler)
pthread_mutex_lock(&queue->mutex);
queue->ready = true;
pthread_cond_signal(&queue->cond);
pthread_mutex_unlock(&queue->mutex);

// Consommateur (worker)
pthread_mutex_lock(&queue->mutex);
while (!queue->ready) {
    pthread_cond_wait(&queue->cond, &queue->mutex);
}
// traitement
pthread_mutex_unlock(&queue->mutex);
```

### 4. Pas de callback ncurses depuis un worker

```c
// ✅ Correct : le worker signale au thread principal
void worker_callback(task_t *task, void *userdata) {
    shared_state_t *state = userdata;
    pthread_mutex_lock(&state->mutex);
    state->result_ready = true;
    pthread_cond_signal(&state->cond);
    pthread_mutex_unlock(&state->mutex);
}

// ❌ Interdit : appel ncurses depuis le worker
// void worker_callback(...) {
//     mvprintw(...);
// }
```

## Primitives utilisées

| Primitive | Usage |
|---|---|
| `pthread_mutex_t` | Protection des données partagées |
| `pthread_cond_t` | Synchronisation producteur/consommateur |
| `pthread_create()` | Création des workers |
| `pthread_join()` | Attente de fin des workers |
| `pthread_cancel()` | Annulation d'un worker (dernier recours) |

## Invariants de concurrence

1. **Un seul thread ncurses** : ncurses n'est jamais appelé depuis un
   worker. Toute mise à jour de l'UI passe par des variables partagées
   protégées.

2. **Mutex hiérarchique** : si plusieurs mutex sont acquis, toujours dans
   le même ordre pour éviter les deadlocks.

3. **Annulation coopérative** : les workers vérifient périodiquement un
   drapeau d'annulation. Pas d'interruption brutale sauf dernier recours.

4. **Réservation atomique** : la réservation du gouverneur est atomique.
   Deux threads ne peuvent pas obtenir la même réservation.

5. **Pas de callback sans réservation** : aucun callback de tâche n'est
   invoqué sans réservation active. Cet invariant est maintenu même en
   présence d'erreurs.

## Anti-patterns

### Deadlock

```c
// ❌ Risque de deadlock
pthread_mutex_lock(&mutex_a);
pthread_mutex_lock(&mutex_b);  // attend mutex_b

// Dans un autre thread :
pthread_mutex_lock(&mutex_b);
pthread_mutex_lock(&mutex_a);  // attend mutex_a → DEADLOCK
```

**Solution** : toujours acquérir les mutex dans le même ordre.

### Race condition

```c
// ❌ Race condition
if (task->state == TASK_STATE_QUEUED) {
    task->state = TASK_STATE_RUNNING;
}

// ✅ Correct
pthread_mutex_lock(&task->mutex);
if (task->state == TASK_STATE_QUEUED) {
    task->state = TASK_STATE_RUNNING;
}
pthread_mutex_unlock(&task->mutex);
```

### Use-after-free

```c
// ❌ Use-after-free
task_destroy(task);
task_callback(task);  // task est libéré

// ✅ Correct
task_callback(task);
task_destroy(task);
```

## Validation

Pour tout ticket touchant la concurrence, exécuter :

```sh
# Build TSan
meson setup build-tsan --wipe -Db_sanitize=thread -Db_static=false
meson compile -C build-tsan -j8
meson test -C build-tsan --print-errorlogs
```

TSan détecte automatiquement :
- les race conditions
- les deadlocks potentiels
- les signaux perdus
- les verrous non libérés

## Checklist de concurrence

Avant de livrer un ticket touchant la concurrence :

- [ ] Toutes les variables partagées sont protégées par un mutex
- [ ] Les mutex sont toujours libérés (même en cas d'erreur)
- [ ] Les variables de condition sont vérifiées dans une boucle `while`
- [ ] Aucun appel ncurses depuis un worker
- [ ] L'annulation est coopérative (pas de `pthread_cancel` sauf dernier recours)
- [ ] TSan ne signale aucune erreur
- [ ] Le build ASan ne signale aucune fuite mémoire liée aux threads

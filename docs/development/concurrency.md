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

SSD operation thread (0 ou 1, joinable)
└── Un poll ou contrôle UDisks synchrone borné, sans ncurses ni Task
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
// Producteur (caller de la Task Queue)
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
| `pthread_cancel()` | Non utilisé pour interrompre une Task ; annulation coopérative |

## Invariants de concurrence

1. **Un seul thread ncurses** : ncurses n'est jamais appelé depuis un
   worker. Toute mise à jour de l'UI passe par des variables partagées
   protégées.

2. **Mutex hiérarchique** : si plusieurs mutex sont acquis, toujours dans
   le même ordre pour éviter les deadlocks.

3. **Annulation coopérative** : les workers vérifient périodiquement un
   drapeau d'annulation. Une Task n'est pas interrompue brutalement.

4. **Réservation atomique** : la réservation du gouverneur est atomique.
   Deux threads ne peuvent pas obtenir la même réservation.

5. **Pas de callback sans réservation** : aucun callback de tâche n'est
   invoqué sans réservation active. Cet invariant est maintenu même en
   présence d'erreurs.

6. **Retraite après callback** : la notification terminale finit avant la
   destruction du userdata. Queue détruit la Task hors de son mutex et ne
   conserve ensuite qu'un snapshot borné.

7. **Fermeture d'ingress** : le propriétaire empêche les nouveaux appels Queue
   avant `destroy()`. La fermeture interne attend le worker et chaque appel
   enregistré avant le close ; elle ne peut rendre sûr un appel démarré après
   la libération d'un pointeur C brut.

8. **Parallélisme scientifique propriétaire** : lorsqu'un kind emploie des
   participants internes, le callback Queue demeure l'unique propriétaire. Le
   nombre de participants et leur mémoire sont admis par le Governor ; seul le
   propriétaire publie le préfixe durable ordonné et joint tous les enfants.

9. **Lease SSD par objet** : un lease scratch appartient à l'adresse exacte de
   l'objet fourni par le caller. Tous ses champs sont lus/écrits sous le mutex
   du contrôleur. Le caller lui garantit un accès exclusif et ne le copie, ne le
    déplace ni ne le présente simultanément à deux contrôleurs. En production,
    acquire/release passent par les wrappers Governor ; le Governor relâche son
    mutex avant l'appel contrôleur, et le contrôleur ne rappelle jamais le
    Governor. À la saturation légale `generation == UINT64_MAX`, seule la fin
    du wrapper exact déjà sérialisé peut réconcilier sa propre opération et le
    compte fondé sur les adresses ; une update publique au même watermark ne
    peut pas rendre une autorité stale.

10. **Owner SSD unique** : la TUI/main demande et poll l'opération ; au plus un
    thread joinable exécute une opération bornée et ne touche jamais ncurses.
    Le destroy le joint avant unregister. Une observation malformée enregistre
    `ERROR` et ne confère aucune autorité de contrôle ou de lease.

11. **Frontière projet** : les vues libèrent leurs borrows, puis la Queue est
    annulée/jointe/détruite avant Project DB. Une Queue vide est créée ensuite.
    Aucun callback terminal ne peut donc déréférencer une DB déjà fermée et
    l'histoire d'un projet ne fuit pas dans le suivant.

12. **Ordre d'arrêt global** : Queue et leases Task, puis fermeture projet,
    join/unregister du binding SSD, contrôleur SSD, et enfin Governor. Un
    unregister encore bloqué par un lease est un échec observable, jamais un
    pointeur abandonné.

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

// ✅ Correct : le callback est entièrement revenu avant la destruction
task_callback(task);
task_destroy(task);
```

## Validation

Les readers Visual Index sont sans état partagé mutable. Une query copie la
liste bornée des segments sous le mutex DB, puis effectue hash, lectures et
accumulation après déverrouillage. Un update ne rend le nouveau segment visible
qu'au commit memberships+segment ; une query en cours garde son snapshot.

Pour tout ticket touchant la concurrence, exécuter :

```sh
# Build TSan
CC=clang meson setup build-tsan --wipe -Db_sanitize=thread -Db_lundef=false
meson compile -C build-tsan -j8
meson test -C build-tsan --print-errorlogs
```

TSan détecte automatiquement :

- les accès concurrents conflictuels instrumentés ;
- certaines utilisations incohérentes des primitives de synchronisation.

Il ne prouve pas l'absence de deadlock, de signal perdu ou de bug dans une
bibliothèque non instrumentée. Les invariants de lifetime et d'ordre de locks
restent donc soumis aux tests déterministes et à la revue.

### Preuve TSan globale courante

La matrice fraîche emploie GCC/G++ 16.2.1 et désactive explicitement Vulkan.
Elle passe 14/14 cibles couvrant Task, Project, Queue, Governor, registre/leases
SSD, contrôleur SSD, observateur/TUI async, Candidate, Visual Index, Feature,
Matcher et GV, puis 220/220 répétitions déterministes : **234/234** au total.

La seule liste de suppressions est `tests/tsan-opencv.supp`, limitée aux objets
partagés externes non instrumentés `libopencv_features.so`,
`libopencv_core.so` et `libtbb.so`. Elle ne masque aucune frame Lardon3D. Les
warnings GCC `-Wmaybe-uninitialized` des contrôles OpenCV Feature/SIFT sont
classés non matériels : le callback fournit une Task non nulle et le helper
initialise la structure avant toute autre sortie d'échec. Les warnings OpenCV
du build GV appartiennent aux headers externes.

Cette preuve TSan ne vaut pas validation de concurrence Vulkan. Le backend
ORB Vulkan réel est couvert séparément par le build Clang Vulkan-on 939/939,
la suite 65/65 et ses tests de backend/handle/publication ; cette séparation
doit rester explicite dans tout rapport.

## Checklist de concurrence

Avant de livrer un ticket touchant la concurrence :

- [ ] Toutes les variables partagées sont protégées par un mutex
- [ ] Les mutex sont toujours libérés (même en cas d'erreur)
- [ ] Les variables de condition sont vérifiées dans une boucle `while`
- [ ] Aucun appel ncurses depuis un worker
- [ ] L'annulation des Tasks est coopérative (pas de `pthread_cancel`)
- [ ] TSan ne signale aucune erreur
- [ ] Le build ASan ne signale aucune fuite mémoire liée aux threads

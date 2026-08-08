# File de tâches (Task Queue)

## Objectif et responsabilités

Le module `task_queue` implémente la file d'attente FIFO des tâches de
traitement. Il orchestre l'exécution séquentielle des tâches via un worker
unique, gère la sélection de la prochaine tâche admissible et transmet les
callbacks de résultat.

La file est le point central entre le scheduler (soumission) et les workers
(exécution). Elle ne décide jamais des ressources — elle applique uniquement
l'ordre FIFO et consulte le gouverneur via le scheduler.

## Fichiers

- `include/lardon3d/task_queue.h` — types publics et API
- `src/task_queue.c` — implémentation
- `tests/test_task_queue.c` — tests unitaires

## Types principaux

```c
typedef struct task_queue task_queue_t;

typedef void (*task_callback_fn)(task_t *task, task_result_t *result, void *userdata);
```

## API publique

| Fonction | Description |
|---|---|
| `task_queue_create()` | Alloue et initialise une file vide |
| `task_queue_destroy()` | Libère la file et toutes les tâches restantes |
| `task_queue_submit()` | Soumet une tâche à la file (FIFO) |
| `task_queue_next()` | Sélectionne la prochaine tâche admissible |
| `task_queue_pop()` | Retire et retourne la tâche sélectionnée |
| `task_queue_cancel()` | Annule une tâche spécifique dans la file |
| `task_queue_cancel_all()` | Annule toutes les tâches en attente |
| `task_queue_size()` | Retourne le nombre de tâches en attente |
| `task_queue_is_empty()` | Vérifie si la file est vide |
| `task_queue_set_callback()` | Définit le callback pour les résultats |

## Comportement FIFO

1. `task_queue_submit()` ajoute la tâche en fin de file.
2. `task_queue_next()` parcourt la file du début vers la fin.
3. La première tâche en état `QUEUED` (non en pause, non annulée) est
   retournée.
4. Si aucune tâche n'est admissible, `task_queue_next()` retourne `NULL`.
5. L'ordre de soumission est toujours respecté entre tâches de même priorité.

## Sélection adaptative

`task_queue_next()` saute les tâches en état `WAIT` (en attente de
ressources) et retourne la première tâche réellement admissible. Cela évite
le blocage par la tête de file lorsqu'une tâche ne peut pas démarrer.

## Invariants

1. **Worker unique** : une seule tâche s'exécute à la fois. Pas de parallélisme
   interne à la file.
2. **FIFO strict** : l'ordre de soumission détermine l'ordre d'exécution.
3. **Réservation obligatoire** : aucune tâche n'est exécutée sans réservation
   validée par le gouverneur.
4. **Callback unique** : chaque tâche reçoit exactement un callback (succès,
   échec ou annulation).
5. **Annulation sûre** : annuler une tâche en cours la met en état
   `CANCELLED` sans interrompre brutalement le worker.
6. **Nettoyage complet** : `task_queue_destroy()` libère toutes les tâches
   restantes, y compris celles en cours d'exécution.

## Interactions

- **task** : chaque entrée de la file est un `task_t` avec son état et sa
  progression.
- **scheduler** : le scheduler appelle `task_queue_submit()` et orchestre
  l'exécution via le worker.
- **resource_governor** : la file consulte le gouverneur (via le scheduler)
  avant d'exécuter chaque tâche.
- **hardware_profile / resource_snapshot** : informations matériel utilisées
  par le gouverneur pour les réservations.

## Statut

**IMPLÉMENTÉ** — file FIFO avec worker unique, sélection adaptative, pause et
annulation coopératives.

## Limites

- Worker unique : pas de parallélisme interne.
- Pas de DAG ni de dépendances inter-tâches.
- Pas de priorités (FIFO strict).
- Pas de persistance des tâches après arrêt.
- Pas de pool de workers CPU/IO/GPU.
- Pas de contre-pression (backpressure) entre étapes.

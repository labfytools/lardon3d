# Système de tâches (Task System)

## Objectif et responsabilités

Le module `task` gère le cycle de vie complet des tâches de traitement dans
Lardon3D. Il définit les états, la progression, la pause coopérative,
l'annulation et les callbacks associés à chaque tâche.

Chaque tâche représente une unité de travail atomique : estimation des
coûts, exécution sous réservation et publication d'un résultat validé.

## Fichiers

- `include/lardon3d/task.h` — types publics et API
- `src/task.c` — implémentation

## Types principaux

```c
typedef enum {
    TASK_STATE_IDLE,
    TASK_STATE_QUEUED,
    TASK_STATE_RUNNING,
    TASK_STATE_PAUSED,
    TASK_STATE_CANCELLED,
    TASK_STATE_DONE,
    TASK_STATE_FAILED
} task_state_t;

typedef struct {
    uint64_t ram_bytes;
    uint64_t gpu_bytes;
    uint32_t cpu_weight;
    uint32_t io_weight;
    uint32_t batch_size;
    uint32_t batch_max;
} task_estimate_t;
```

## API publique

| Fonction | Description |
|---|---|
| `task_create()` | Alloue et initialise une tâche avec son estimate |
| `task_destroy()` | Libère toutes les ressources de la tâche |
| `task_get_state()` | Retourne l'état courant (thread-safe en lecture) |
| `task_set_state()` | Met à jour l'état avec transitions validées |
| `task_get_estimate()` | Retourne l'estimation immuable des coûts |
| `task_advance_progress()` | Avance la progression d'un pas validé |
| `task_request_pause()` | Demande une pause coopérative |
| `task_request_cancel()` | Demande une annulation coopérative |
| `task_should_pause()` | Vérifie si la tâche doit se mettre en pause |
| `task_should_cancel()` | Vérifie si la tâche doit s'annuler |

## Invariants

1. **Estimation immuable** : une fois créée, l'estimation d'une tâche ne change
   jamais. Elle est copiée en lecture seule lors de la réservation.
2. **Transitions d'état validées** : seules certaines transitions sont
   autorisées (IDLE → QUEUED → RUNNING → DONE/FAILED).
3. **Pause et annulation coopératives** : le worker vérifie périodiquement
   `task_should_pause()` et `task_should_cancel()`. Le callback ne force jamais
   l'arrêt.
4. **Progression bornée** : la progression ne peut jamais dépasser la valeur
   maximale définie par l'estimation.
5. **Callback unique** : chaque tâche possède un seul callback invoqué une
   seule fois, quelle que soit l'issue.

## Interactions

- **task_queue** : la file gère l'ordre d'exécution et invoque les callbacks.
- **resource_governor** : l'estimation est utilisée pour la réservation avant
  exécution.
- **scheduler** : le scheduler transmet l'estimation lors de la soumission.

## Statut

**IMPLÉMENTÉ** — cycle de vie complet, pause et annulation coopératives.

## Limites

- Aucune priorité interne : l'ordre est uniquement FIFO.
- Aucune persistance : les tâches disparaissent à l'arrêt du programme.
- Aucune dépendance inter-tâches (pas de DAG).
- La progression est linéaire : pas de séquençage adaptatif interne.

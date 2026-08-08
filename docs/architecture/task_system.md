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

Les états réels sont `TASK_PENDING`, `TASK_RUNNING`, `TASK_PAUSED`,
`TASK_CANCELLED`, `TASK_FAILED` et `TASK_COMPLETED`. Une rupture de séquence est
une opération de réadmission, pas un état supplémentaire.

## API publique

| Fonction | Description |
|---|---|
| `lardon3d_task_create()` | Alloue une tâche et copie son estimation |
| `lardon3d_task_destroy()` | Annule, attend puis libère la tâche |
| `lardon3d_task_start()` | Exécute sous réservation active |
| `lardon3d_task_pause()` / `resume()` | Contrôle la pause coopérative |
| `lardon3d_task_request_cancel()` | Demande l'annulation coopérative |
| `lardon3d_task_checkpoint()` | Frontière coopérative en mémoire |
| `lardon3d_task_sequence_break()` | Libère puis renouvelle la réservation |
| `lardon3d_task_snapshot()` | Copie l'état d'observation runtime |

### API durable

| Fonction | Description |
|---|---|
| `lardon3d_task_durable_snapshot()` | Copie les champs durables sous mutex |
| `lardon3d_task_restore()` | Reconstruit une tâche sans état d'exécution vivant |
| `lardon3d_task_checkpoint_save()` | Publie atomiquement un snapshot v1 |
| `lardon3d_task_checkpoint_load()` | Lit et valide un checkpoint borné |

Après `rename`, un échec du `fsync` du répertoire retourne
`LARDON3D_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE` : la publication est visible,
mais sa durabilité après crash n'est pas confirmée.

## Invariants

1. **Estimation immuable** : une fois créée, l'estimation d'une tâche ne change
   jamais. Elle est copiée en lecture seule lors de la réservation.
2. **Transitions d'état validées** : le cycle nominal est
   `PENDING → RUNNING → COMPLETED/FAILED/CANCELLED`, avec pause coopérative.
3. **Pause et annulation coopératives** : le callback appelle périodiquement
   `lardon3d_task_checkpoint()`. Aucun autre thread ne force son arrêt.
4. **Progression bornée** : la progression ne peut jamais dépasser la valeur
   maximale définie par l'estimation.
5. **Reprise réadmise** : une tâche restaurée non terminale repasse par la file,
   le scheduler et le Resource Governor avec une nouvelle réservation.
6. **Snapshot court** : seuls les champs durables sont copiés sous le mutex ;
   la sérialisation et les I/O ont lieu après déverrouillage.

## Interactions

- **task_queue** : la file gère l'ordre d'exécution et invoque les callbacks.
- **resource_governor** : l'estimation est utilisée pour la réservation avant
  exécution.
- **scheduler** : le scheduler transmet l'estimation lors de la soumission.

## Statut

**IMPLEMENTED** — cycle de vie, pause/annulation coopératives, séquences
adaptatives et fondation de checkpoints persistants isolés.

**NOT_YET_WIRED** — sauvegarde automatique et restauration par la file.

La Project Database v1 peut enregistrer transactionnellement un résumé
`Lardon3DTaskDurableSnapshot` et la référence de son checkpoint. Elle ne stocke
ni estimation sérialisée complète, ni callback, ni réservation, et ne remplace
pas la validation du fichier checkpoint avant `task_restore()`.

`lardon3d_project_checkpoint_task()` est la frontière runtime : elle capture le
snapshot, publie le fichier hors mutex de tâche, puis met à jour la DB. L'API
d'inventaire retourne des snapshots durables validés, mais ne peut pas appeler
`task_restore()` tant que le type métier ne sait pas reconstruire callback et
userdata.

## Limites

- Aucune priorité interne : l'ordre est uniquement FIFO.
- Pas encore de reprise globale au démarrage du projet.
- Aucune dépendance inter-tâches (pas de DAG).
- Pas encore de références d'artefacts métier validés.

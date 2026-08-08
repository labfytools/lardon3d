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
| `lardon3d_task_create_typed()` | Crée une tâche persistable avec kind/version immuables |
| `lardon3d_task_restore_typed()` | Restaure une tâche typée et transfère l'ownership du userdata |
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

**IMPLEMENTED** — une registry statique peut reconstruire explicitement le
callback et le userdata d'un kind connu. Le destructeur du userdata est détenu
par la tâche restaurée et exécuté après arrêt de son exécution.

**IMPLEMENTED** — `import.images` utilise la pause, l'annulation, les ruptures
de séquence et les checkpoints génériques ; une tâche restaurée conserve son ID
lors de sa soumission explicite à la file.

**IMPLEMENTED** — resoumission automatique sélective des tâches production à
l'ouverture du projet.

**NOT_YET_WIRED** — autosave générique et dépendances entre tâches.

Le chemin de production de l'import ne possède plus de thread ni de drapeau
d'annulation privés. Son wrapper TUI ne fait qu'enqueue/cancel/observer la
tâche générique. Chaque callback traite un lot borné, checkpoint hors mutex de
tâche, puis effectue une rupture de séquence afin d'obtenir un nouveau contrat
et une nouvelle réservation.

Le callback terminal optionnel est notifié exactement une fois pour
`COMPLETED`, `FAILED` ou `CANCELLED`, jamais pour une pause ou une rupture de
séquence. L'état est fixé sous mutex, puis la réservation terminale est libérée
avant l'appel hors mutex. `join()` attend la fin du callback ; le userdata reste
donc valide pendant celui-ci et son destructeur n'est appelé qu'ensuite par la
destruction de la tâche.

Une tâche reconstruite mais refusée avant transfert à la queue est abandonnée
localement : son userdata est détruit, sans callback terminal ni écriture
durable d'une fausse annulation. Une annulation explicitement demandée conserve
le contrat de notification terminale.

La Project Database v4 peut enregistrer transactionnellement un résumé
`Lardon3DTaskDurableSnapshot` et la référence de son checkpoint. Elle ne stocke
ni estimation sérialisée complète, ni callback, ni réservation, et ne remplace
pas la validation du fichier checkpoint avant `task_restore()`.

`lardon3d_project_checkpoint_task()` est la frontière runtime : elle capture le
snapshot, publie le fichier hors mutex de tâche, puis met à jour la DB. L'API
d'inventaire retourne des snapshots durables validés, mais ne peut pas appeler
`task_restore()` que via un descriptor connu ; aucun pointeur n'est persistant.

## Limites

- Aucune priorité interne : l'ordre est uniquement FIFO.
- Pas encore de DAG pour ordonner des reprises interdépendantes.
- Aucune dépendance inter-tâches (pas de DAG).
- Pas encore de références d'artefacts métier validés.

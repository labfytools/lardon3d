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

Le type public réel est l'opaque `Lardon3DTaskQueue`.

## API publique

| Fonction | Description |
|---|---|
| `lardon3d_task_queue_create()` | Crée une file bornée et son worker |
| `lardon3d_task_queue_destroy()` | Arrête, annule, attend puis libère la file |
| `lardon3d_task_queue_add()` | Ajoute avec backpressure bloquante |
| `lardon3d_task_queue_try_add()` | Ajoute sans bloquer si une place existe |
| `lardon3d_task_queue_try_add_ex()` | Distingue succès, saturation, arrêt, collision d'ID et erreur |
| `lardon3d_task_queue_pause()` / `resume()` | Contrôle une tâche par son ID stable |
| `lardon3d_task_queue_cancel()` | Demande l'annulation par ID |
| `lardon3d_task_queue_remove()` | Retire une tâche terminale |
| `lardon3d_task_queue_snapshot()` | Copie une vue bornée de la file |

## Comportement FIFO

1. `lardon3d_task_queue_add()` ajoute la tâche en fin de file.
2. Le sélecteur interne parcourt la file du début vers la fin.
3. La première tâche `PENDING` admissible (non terminale) est
   retournée.
4. Si aucune tâche n'est admissible, le worker attend un changement.
5. L'ordre de soumission est toujours respecté entre tâches de même priorité.

## Sélection adaptative

Le sélecteur saute les tâches pour lesquelles le governor répond `WAIT` et
retourne la première tâche réellement admissible. Cela évite
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

- **task** : chaque entrée de la file est un `Lardon3DTask` avec son état et sa
  progression.
- **scheduler** : la file applique la soumission FIFO et orchestre
  l'exécution via le worker.
- **resource_governor** : la file consulte le gouverneur (via le scheduler)
  avant d'exécuter chaque tâche.
- **hardware_profile / resource_snapshot** : informations matériel utilisées
  par le gouverneur pour les réservations.

## Statut

**IMPLÉMENTÉ** — file FIFO avec worker unique, sélection adaptative, pause,
annulation coopérative et accueil des tâches restaurées avec ID préassigné.

La reprise projet utilise `try_add_ex()` et ne bloque jamais `project_open()`.
À saturation, elle arrête sa fenêtre : les tâches non transférées restent
`PENDING` en DB et seront réévaluées lors d'une ouverture ultérieure. Ce n'est
pas un second scheduler.

## Limites

- Worker unique : pas de parallélisme interne.
- Pas de DAG ni de dépendances inter-tâches.
- Pas de priorités (FIFO strict).
- La reprise ne possède pas encore de DAG ni de déclenchement différé lorsque
  une place se libère pendant la session courante.
- Pas de pool de workers CPU/IO/GPU.
- La backpressure borne les producteurs à la capacité configurée.

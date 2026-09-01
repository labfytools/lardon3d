# File de tâches (Task Queue)

## Objectif et responsabilités

Le module `task_queue` implémente la file d'attente FIFO des tâches de
traitement. Il orchestre l'exécution séquentielle des tâches via un worker
unique, gère la sélection de la prochaine tâche admissible et transmet les
callbacks de résultat.

La file est le point central entre les producteurs et le worker d'exécution.
Les responsabilités historiques de scheduler sont portées par cette Queue et
le runtime existants : aucun second scheduler n'existe. La Queue ne décide
jamais des ressources ; elle demande l'admission au Governor.

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
| `lardon3d_task_queue_remove()` | Retire un snapshot terminal retenu |
| `lardon3d_task_queue_snapshot()` | Copie une vue bornée de la file |
| `lardon3d_task_queue_observe()` | Copie la vue additive typée/durable/admission |

## Comportement FIFO avec bypass stable des WAIT

1. `lardon3d_task_queue_add()` ajoute la tâche en fin de file.
2. Le sélecteur interne parcourt la file du début vers la fin.
3. La première tâche `PENDING` admissible (non terminale) est
   retournée.
4. Si aucune tâche n'est admissible, le worker attend un changement.
5. L'ordre de soumission est respecté entre tâches admissibles ; une tâche en
   `WAIT` ressources peut être dépassée sans être réordonnée ou supprimée.

Lorsqu'au moins une tâche PENDING reste en `WAIT` de ressources, cette attente
est temporisée à 500 ms maximum. À l'expiration, le worker reprend le scan stable
depuis la tête et le chemin d'admission capture de nouveaux snapshots. Enqueue,
resume, `resources_changed`, annulation et arrêt continuent de réveiller
immédiatement le worker ; le timeout n'impose donc jamais 500 ms après un signal.
Cette règle utilise le worker unique existant et n'ajoute aucun thread.

## Sélection adaptative

Le sélecteur saute les tâches pour lesquelles le governor répond `WAIT` et
retourne la première tâche réellement admissible. Cela évite
le blocage par la tête de file lorsqu'une tâche ne peut pas démarrer.

## Invariants

1. **Worker unique** : une seule tâche s'exécute à la fois. Pas de parallélisme
   interne à la file.
2. **FIFO stable** : le scan commence à la tête et ne contourne que les
   `WAIT` d'admission. Il n'existe ni priorité cachée ni réordonnancement.
3. **Réservation obligatoire** : aucune tâche n'est exécutée sans réservation
   validée par le gouverneur.
4. **Notification terminale unique** : le callback terminé de la Task retourne
   avant la destruction de son userdata. Les callbacks métier peuvent avoir
   plusieurs séquences admises.
5. **Annulation sûre** : annuler une tâche en cours la met en état
   `CANCELLED` sans interrompre brutalement le worker.
6. **Retraite prompte et bornée** : après retour du callback terminé, la vraie
   Task est détruite hors mutex et seule une copie de snapshot demeure. Les 64
   terminaisons les plus récentes sont retenues ; les plus anciennes sont
   évincées sans conserver userdata ni Task.
7. **Fermeture coordonnée** : `destroy()` ferme d'abord l'ingress, attend le
   worker et tout appel enregistré avant la fermeture, puis libère mutex et
   mémoire exactement une fois. Le propriétaire doit interdire tout nouvel
   appel API dès que la destruction commence.
8. **IDs non réutilisés** : la génération est monotone. Après consommation ou
   génération de `UINT64_MAX`, l'épuisement est permanent pour la vie de la
   Queue ; un ID restauré plus petit ou une éviction d'historique ne la réarme
   jamais.
9. **Observation complète bornée** : la Queue de production accepte au plus 64
   pending, possède au plus une active et retient 64 terminaux. Une capacité
   additive de 129 observe donc tout l'état courant. Les Tasks vivantes sont
   copiées d'abord en ordre de soumission décroissant, puis l'histoire en ordre
   de terminaison décroissant ; l'actif ne peut pas être caché par l'histoire.

## Interactions

- **task** : chaque entrée de la file est un `Lardon3DTask` avec son état et sa
  progression.
- **runtime / producteurs** : soumettent les Tasks ; la Queue applique FIFO et
  orchestre l'exécution via le worker.
- **resource_governor** : la file consulte directement le gouverneur avant
  d'exécuter chaque tâche.
- **hardware_profile / resource_snapshot** : informations matériel utilisées
  par le gouverneur pour les réservations.

## Statut

**GATE G — PASS / FROZEN** — file FIFO stable avec worker unique,
sélection adaptative, réévaluation autonome des `WAIT`, pause, annulation
coopérative et accueil des tâches restaurées avec ID préassigné.

La reprise projet utilise `try_add_ex()` et ne bloque jamais `project_open()`.
À saturation, elle arrête sa fenêtre : les tâches non transférées restent
`PENDING` en DB et seront réévaluées lors d'une ouverture ultérieure. Ce n'est
pas un second scheduler.

Ouvrir, fermer ou changer de projet détruit/joint cette Queue avant fermeture
de Project DB, puis recrée une unique Queue vide. L'histoire et son namespace
d'IDs ne traversent donc jamais une frontière projet ; ce reset ne modifie pas
les Task IDs durables conservés dans chaque DB.

Les callbacks terminés sont appelés sans le mutex Queue. Tant que le
propriétaire maintient la Queue vivante, ils peuvent employer les lectures
`get`, `get_at`, `count` et `snapshot`. Ils ne doivent pas appeler
synchroniquement `remove()` sur leur propre record, `destroy()` sur la même
Queue, ni une opération dont l'achèvement dépend de leur propre retour.

## Limites

- Worker unique : pas de parallélisme inter-Tasks. Un kind peut posséder un
  parallélisme interne borné et chargé dans sa réservation.
- Pas de DAG ni de dépendances inter-tâches.
- Pas de priorités (FIFO stable avec bypass des seuls `WAIT`).
- La reprise ne possède pas encore de DAG ni de déclenchement différé lorsque
  une place se libère pendant la session courante.
- Pas de pool de workers CPU/IO/GPU.
- La backpressure borne les producteurs à la capacité configurée.
- Les 500 ms concernent uniquement l'admission initiale PENDING. Le polling
  existant à une rupture de séquence reste 50 ms.

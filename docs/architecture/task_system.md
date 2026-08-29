# Système de tâches (Task System)

## Extraction précise v1A

`features.extract.sift` et `features.extract.rootsift`, version 1, possèdent
chacun leur lifecycle durable. Une tâche correspond à une image, est persistée
avant enqueue et reprend avec le même `task_id`. `detectAndCompute` n'est pas
interruptible : pause/cancel sont coopératifs à ses frontières et un crash
recommence uniquement cette image. ORB READY n'est jamais recalculé par SIFT.

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
| `lardon3d_task_checkpoint_stage()` / `promote_staged()` | Publie d'abord `<path>.next`, puis promeut sous le verrou par tâche |

Après `rename`, un échec du `fsync` du répertoire retourne
`LARDON3D_TASK_CHECKPOINT_PUBLISHED_NOT_DURABLE` : la publication est visible,
mais sa durabilité après crash n'est pas confirmée.

### Publication et reprise projet

Le protocole générique par tâche emploie le chemin canonique
`.lardon3d/checkpoints/<task_id>.chk`, sa représentation staged `.chk.next` et
le verrou consultatif `.chk.lock`. Le verrou sérialise un writer et la reprise
sur le slot staged fixe ; il est détenu pour la séquence entière et sa fermeture
par le noyau après crash ne constitue pas un état durable.

L'ordre de publication est strictement : codec durable de `.next`, commit
SQLite du résumé de tâche et de la référence checkpoint, puis promotion de
`.next` vers le fichier canonique et durabilité du répertoire. DB et système de
fichiers ne forment pas une transaction unique : si la promotion échoue ou sa
durabilité est incertaine après le commit SQLite, `.next` reste la
représentation de récupération possible.

La reprise acquiert d'abord `.lock`, puis recharge la ligne SQLite : la page de
découverte peut être devenue périmée pendant l'attente. Elle valide le codec et
la version du canonique et compare seulement les champs effectivement stockés
dans le résumé DB — `task_id`, nom, états saved/recovery, progression et
compteur de séquences. Les timestamps et l'estimation complète ne sont pas
dupliqués par la DB et ne participent donc pas à ce test. Un canonique valide
dont ce résumé correspond est prioritaire ; une `.next` absente, périmée ou
corrompue est alors ignorée. Sinon, une `.next` valide qui correspond au même
résumé est promue sous le même verrou puis reprise. Un ancien projet v22 qui ne
possède que `.chk` reste ainsi récupérable. Toute autre absence, version non
supportée, corruption ou divergence interdit la reprise de cette tâche sans
affecter les autres entrées de l'inventaire.

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

**IMPLEMENTED** — `visual_index.update` traite jusqu'à seize Feature Sets par
séquence, checkpoint après commit de segment et repasse par le Governor.

**IMPLEMENTED** — `candidate_pair.generate` traite jusqu'à soixante-quatre
Feature Sets par séquence, interroge le Visual Index, persiste les paires
candidates avec idempotence, checkpoint après chaque lot et repasse par le
Governor via `sequence_break`. La reprise est idempotente avec le curseur
`after_feature_set_id` rechargé depuis la DB.

**IMPLEMENTED** — `matcher.run` traite une Candidate Pair atomique à la fois,
par lots adaptatifs de 1, 2, 4 ou 8. Il persiste le curseur
`after_candidate_pair_id`, checkpoint après publication de chaque lot et
effectue une rupture de séquence avant le suivant. Une paire repassée après un
crash est réutilisée par son Match Result.

**IMPLEMENTED** — `geometric_verifier.run` v1 traite un Match Result atomique à
la fois, par lots adaptatifs de 1, 2, 4 ou 8. Project DB v13 conserve sa
configuration scientifique et `after_match_result_id`. Chaque résultat est
publié avant le curseur ; pause, annulation, checkpoint et rupture de séquence
restent coopératifs aux frontières des parents et des lots.

**IMPLEMENTED** — `track_builder.run` v1 est une tâche durable de rebuild
complet. Le scope GVR est persistant et immuable ; la reprise rejoue depuis le
début avant publication et l'exact reuse Gate C absorbe un crash post-publication.
Pause et annulation sont observées avant/after l'unité Gate B non préemptible.

**PASS / FROZEN** — `acquisition_campaign.run` v1 est une
tâche durable reconstruite par la registry existante à partir de son Task ID et
de sa requête typée immuable Project DB v20. Elle matérialise un seul groupe
S3-E par séquence, vérifie pause/annulation aux frontières de groupe, retient
transactionnellement Capture et curseur avant progression/checkpoint, puis
appelle `sequence_break` avant le groupe suivant. Sa reprise passe par la Queue
et le Resource Governor existants ; aucune boucle d'exécution parallèle n'est
introduite.

**PASS / FROZEN** — le callback `photo_quality.triage` v1 réutilise ce
même Task/Queue/Resource Governor et une `sequence_break` entre groupes. Sa requête typée
est immuable ; le `next_group_id` canonique commence à 1, avance à `k+1` après le résultat
`k`, puis vaut `N+1` à terminaison. Résultat et curseur sont durables avant le checkpoint
générique, de sorte qu'une reprise ne devine ni ne réanalyse une identité déjà publiée. La
réservation charge le contexte retenu et 20 MiB de travail par groupe ; le
JPEG au-dessus de la limite opérationnelle de décodage 8192 pixels reste en attente
`UNAVAILABLE + SUSPECT` (ni erreur de décodage ni rejet) ; une entrée admise est réduite à
1024 pixels maximum avant analyse. Cette borne ne limite ni la campagne ni le dataset.

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

La Project Database v7 peut enregistrer transactionnellement un résumé
`Lardon3DTaskDurableSnapshot` et la référence de son checkpoint. Elle ne stocke
ni estimation sérialisée complète, ni callback, ni réservation, et ne remplace
pas la validation du fichier checkpoint avant `task_restore()`.

`lardon3d_project_checkpoint_task()` est la frontière runtime : elle capture le
snapshot, stage le fichier hors mutex de tâche, met à jour la DB, puis promeut
le fichier canonique sous le verrou par tâche. L'API d'inventaire retourne des
snapshots dont le codec/version et le résumé DB ont été validés, mais ne peut
appeler `task_restore()` que via un descriptor connu ; aucun pointeur n'est
persistant.

## Limites

- Aucune priorité interne : l'ordre est uniquement FIFO.
- Pas encore de DAG pour ordonner des reprises interdépendantes.
- Aucune dépendance inter-tâches (pas de DAG).
- Pas encore de références d'artefacts métier validés.

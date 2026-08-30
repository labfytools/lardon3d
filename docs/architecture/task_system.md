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

1. **Estimation durable immuable** : l'estimation stockée dans la Task courante
   ne change jamais. Compute Governor v2 utilise une enveloppe privée de
   capacités, distincte de ce snapshot et non persistée comme identité. Le
   contrat choisi pour une séquence est lui aussi immutable jusqu'à sa
   libération ; seule une séquence suivante peut être adaptée.
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
- **scheduler** : ses responsabilités restent représentées par le runtime et
  l'unique Queue existants ; aucun second scheduler n'est introduit.

### Frontière Compute Governor v2

**COMPUTE_GOVERNOR_V2 — PASS / FROZEN.** Chaque kind de production
reste propriétaire d'une Task et passe par l'unique Queue à un worker, puis par
l'unique Resource Governor, même lorsque CPU, lot, mémoire, I/O et GPU sont
tous fixes. Une dimension fixe n'autorise jamais à contourner l'admission.

Le code conserve une `Lardon3DResourceEstimate` canonique immutable pour la
durabilité. Derrière les types opaques, chaque Task possède désormais une
enveloppe privée bornée : par défaut une capacité fixe exactement égale à cette
estimation ; seuls les kinds possédant des alternatives réelles en ajoutent.
Queue et `sequence_break()` demandent au Governor de choisir et réserver une
capacité depuis un seul snapshot courant. L'exécution reçoit ce contrat
immutable et ne le renégocie pas en cours de séquence. Cette couture n'étend ni
le descriptor public du Task Kind Registry, ni le checkpoint, ni le payload
Project DB et n'ajoute aucune dimension d'identité scientifique.

Le Governor possède aussi la politique CPU hôte privée. Depuis le masque
permis et la topologie package/core, il dérive un compute-pool par coeurs
physiques complets ; un caller déjà précontraint fournit directement son pool.
Sans topologie exploitable, le budget portable subsiste avec affinité inactive.
Seul le worker lourd de Queue applique et vérifie son propre masque (`pid=0`)
avant les callbacks, donc le creator/main/TUI reste libre. Le Governor ne mute
jamais un TID auxiliaire énuméré : un `PIDFD_THREAD` ne stabilise pas le numéro
TID consommé par `sched_setaffinity(tid)`. Le démarrage établit plutôt
`MESA_SHADER_CACHE_DISABLE=true` avant tout pthread applicatif et toute
initialisation Vulkan. Une absence prend ce défaut sûr ; `true`/`1` explicites
sont conservés, tandis qu'une valeur explicite fausse ou malformée est respectée
mais entraîne un refus de démarrage. Cette politique opérationnelle, sans état
Task durable ni identité scientifique, supprime les helpers de cache Mesa
observés qui élargissaient leur masque. Il n'existe plus de sweep post-init,
latch de Task ou retry de recontrainte auxiliaire ; les diagnostics indiquent
l'état et la raison de la politique sans prétendre une activité auxiliaire.
Le nombre du pool borne toute admission CPU. Cette couture
n'ajoute ni scheduler, ni worker, ni champ durable ou ABI publique.

Le comportement de production normal ORB est `AUTO` Governor-owned. Les modes
CPU/Vulkan explicites sont réservés au debug, benchmark et à la
reproductibilité. Une Task normale nouvelle persiste une classe `MIXED`
sémantiquement honnête ; cette signature reconstruit AUTO, tandis que toute
signature CPU ORB ancienne ou courante reconstruit un CPU fixe et que Vulkan
reste fixe. Création et reprise n'initialisent pas Vulkan sur le caller ; le
premier begin appartient au worker Queue contraint. Le choix matériel ne
devient ni payload ni identité. Seule une reprise AUTO établit la disponibilité
Vulkan partagée ; les reprises fixes et historiques sont sans effet global,
donc leur ordre ne dégrade pas AUTO. Une panne backend est publiée avant tout
fallback ou sortie précoce ; une inéligibilité de paire reste locale et ne
possède aucun handle à terminer. Le Governor conserve une télémétrie fixe et
bornée par kind/backend (dernière durée, travail durable, débit, backend
sélectionné/réel, contrat, pression et raison) et aucune grande histoire
persistante. L'adaptation générique utilise deux observations de référence et
deux du palier d'essai. Le lot ORB Vulkan, plus bruité, exige huit séquences
pures consécutives pour chacune de ces fenêtres avant toute décision ; une
observation Matcher mesure la cadence de bout en bout depuis la réadmission
réussie jusqu'au checkpoint générique durable, et non le seul callback de
calcul. Le temps de calcul seul reste diagnostique. Ainsi le Governor apprend
le coût de séquence amorti par le lot et un checkpoint échoué ne l'entraîne pas.
Une
admission adaptative encore permise sous pression installe
CPU1, lot minimum et inflight minimum avant sa réservation. Inflight ORB normal
reste fixé à 1; helpers reste 0. Les callbacks atomiques n'annoncent
un item que si extraction et publication propre sont durables ; READY,
`ALREADY_PRESENT` et `PUBLISHED_NOT_DURABLE` sont des observations zéro qui
n'avancent pas la rampe.
L'A/B forcé ABBA a mesuré seulement +2,077617 % à depth 2
(54,661652238 contre 55,797311953 paires/s), sous le deadband 5 %, avec digest identique,
quatre séquences de fallback local par exécution et zéro panne/discard. Depth 2 est donc
**REJECTED_WITH_MEASURED_REASON** pour AUTO normal. Compute Governor v2 reste
en cours jusqu'aux réconciliations restantes.

Pour Matcher Vulkan, le lease privé de capacité matérialise le contrat de la
séquence seulement après son admission. Il alloue un ou deux payloads de
640 Kio sans requête pending, les conserve jusqu'au nettoyage complet de la
séquence, puis rend depth 2 à depth 1 avant `sequence_break()`. Une allocation
depth 2 échouée ne modifie pas le contrat scientifique et conduit à la paire CPU
complète; elle ne laisse ni réservation suivante sous-facturée, ni résultat
Vulkan partiel.

Cette boucle est maintenant `observe → choose → execute → measure → adapt next`.
Les CPU réductibles progressent uniquement par `1/2/4/8/12`, bornés par le
compute-pool ; CPU et lot ne sont jamais essayés ensemble. Les observations
hôte privées comprennent utilisation du pool, mémoire/PSI/swap actif, GPU busy
et RSS observé, sans confondre RSS et réservation. Le dernier diagnostic peut
être tiré par numéro de série ou formaté dans un buffer borné ; le runtime ne
l'imprime pas directement dans le TUI. Le contrat déjà installé demeure
immutable même si une nouvelle observation arrive pendant son exécution.
Un agrégat privé de taille fixe complète ce dernier diagnostic : il compte les
admissions et séquences effectivement enregistrées et somme leurs métriques en
saturant, afin qu'un poller lent ne transforme pas des changements coalescés en
fausse histoire exhaustive. Il vit uniquement avec le Governor courant.
Pour la matrice forcée du runner, cet agrégat sépare les fallbacks par
inéligibilité locale, panne backend et raison autre/inconnue, à la fois par
séquence diagnostique et par item exact. Le compte d'items avance une seule fois
après la publication durable du fallback CPU complet; le travail d'une
séquence sélectionnée CPU reste à zéro. Ce commit opérationnel immédiat survit à
l'échec ou l'annulation d'une paire suivante, mais ne crée ni séquence réussie,
ni débit durable, ni adaptation. Un high-water mark non persisté le rend
idempotent pendant la vie de la Task. La Task n'expose alors qu'une capacité
Vulkan aux batch/depth demandés : absence de GPU/backend/mémoire ne peut donc
pas devenir une admission CPU. Une panne backend tardive laisse la publication
CPU complète déjà durable mais fait échouer la Task de benchmark après
checkpoint; seul le compte d'items localement inéligibles, égal entre cohortes
comparées même lorsque leur batch diffère, reste admissible pour l'évidence.
Les anciens logs batch 2/4, à quatre contre trois séquences locales pour les
mêmes items/digest, sont préliminaires et ne déterminent aucun débit utile. La
matrice item-valide suivante mesure batch 2/4/8/12 à
54,180767704/66,094373197/74,784998723/76,755814095 paires/s. Les deux premiers
gains de palier dépassent le deadband, celui de 8 à 12 vaut seulement
+2,635308425 % : AUTO normal s'arrête à batch 8 et batch 12 reste un contrôle
privé sûr rejeté pour la politique normale.
Le run sans override `short-auto-batch8-governor-v2.stdout.jsonl` valide ensuite
la Task réelle : `1 → 2 → 4 → 8`, 4113/4113 résultats, 76,072 paires/s, digest
identique, six items locaux et aucune panne/discard. La cadence de séquence
inclut réadmission et checkpoint durable; inflight reste 1 et helpers 0.
Le run S21 final crée ensuite la Task normale 2831 en mode AUTO, sans option
backend/lot/inflight. Ses 21 630 admissions sont toutes Vulkan; la Task publie
172 741/172 741 résultats, termine `COMPLETE` à 100 %, avec zéro doublon et
curseur complet. La seule admission YELLOW produit un contrat batch 1, puis
les admissions GREEN suivantes remontent de façon bornée jusqu'à batch 8. Le
contrat actif n'est jamais muté sous le callback : chaque changement appartient
à la séquence suivante.

Le runner d'évidence réel crée les nouvelles Tasks Matcher normales par l'API
AUTO. Son contrôle `synchronous` est un flag de contexte compilé seulement dans
le target benchmark/test : il ne modifie ni Task durable, ni envelope, ni
contrat installé, ni callback de production. Puisque ce flag n'est pas
checkpointé, le runner refuse expressément de l'appliquer à une reprise Matcher
pendante. Le chemin rolling/recovery normal reste inchangé.

Dans ce même target seulement, `--matcher-inflight 1|2` reconstruit avant la
création d'une Task neuve une enveloppe AUTO Vulkan à profondeur fixe et batch
2 par défaut. `--matcher-batch 2|4|8|12`, valable seulement avec inflight et
rolling AUTO, fixe aussi le lot. Ces contrôles ne remplacent pas le Governor : l'admission, la mémoire
par slot, l'UMA et la réservation de séquence restent identiques. Le contrôle
est refusé pour les modes explicites, pour synchronous depth 2 et pour une Task
pendante. Une garde de processus restaure les tokens privés sur toute sortie;
aucun token, champ de contexte, payload ou comportement correspondant n'est
compilé dans `lardon3d`.

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

**PASS / FROZEN — Compute Governor v2.** `matcher.run` traite une
Candidate Pair atomique à la fois, par lots opérationnels bornés jusqu'à 12.
Le code courant consomme honnêtement CPU, lot et GPU du contrat choisi. Feature,
SIFT et RootSIFT appliquent l'admission OpenCV `1..12`; RAW et Photo Quality
restent CPU1. Pour ORB normal, le Governor choisit GPU-first ou CPU complet pour la prochaine
séquence, sans mutation pendant son exécution. Ces dimensions ne changent ni
identité scientifique ni publication. Le callback persiste le curseur
`after_candidate_pair_id`, checkpoint après publication de chaque lot et
effectue une rupture de séquence avant le suivant. Une paire repassée après un
crash est réutilisée par son Match Result. La soumission Vulkan rolling est
privée et request-bound; le contrat normal de séquence fige inflight 1. Deux slots
maximum restent disponibles à la couture privée de sûreté/benchmark et portent
chacun command/fence/buffers/query et un handle exact tandis
que device/pipeline/layout/cache restent partagés. Le propriétaire soumet
jusqu'à la profondeur admise, finit le plus ancien et publie toujours le
préfixe canonique contigu; toute sortie, annulation ou exception nettoie les
handles encore privés. Les buffers du second slot ne sont mappés que pendant
une séquence depth 2 admise et sont libérés avant la rupture suivante. Helpers
reste 0 et le contrôle synchrone force depth 1.

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

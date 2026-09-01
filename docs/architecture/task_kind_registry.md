# Registry des types métier de tâches

## Feature kinds v1A

La registry statique conserve `features.extract` v1 pour ORB et ajoute
`features.extract.sift` v1 et `features.extract.rootsift` v1. Les reconstructeurs
chargent la table dédiée, revalident le fingerprint et ne capturent aucun
`AppState`.

## Contrat

Une instance possède un **task ID** stable. Son **task kind** décrit son
comportement métier, son **task state** décrit son état d'avancement, la
**checkpoint version** décrit le codec générique et la **task kind version**
versionne les paramètres nécessaires au reconstructeur. Ces identités ne sont
pas interchangeables.

Le kind v1 est une chaîne ASCII de 1 à 64 caractères au format
`[a-z0-9][a-z0-9._-]*`. La version est un entier non nul. Aucun kind n'est
déduit d'un nom, callback ou pointeur et aucune normalisation n'est effectuée.

## Registry et ownership

La registry est une vue bornée à 64 descriptors sur un tableau statique
immutable. Le lookup est linéaire, déterministe, sans allocation et sûr en
lecture concurrente. Elle ne charge aucun code dynamiquement.

Un descriptor contient exactement le kind, sa version et un reconstructeur.
Le reconstructeur produit callback, userdata et destructeur optionnel. Avant le
transfert, la registry nettoie le userdata sur toute erreur ; après restauration
réussie, `Lardon3DTask` en devient propriétaire et le détruit une fois après la
fin de l'exécution. Le constructeur métier n'est jamais appelé sous mutex DB.

La registry peut normaliser une ancienne estimation opérationnelle connue. Le
reconstructeur reçoit toujours le snapshot durable original afin de valider le
mode exact ; la registry applique ensuite l'estimation effective uniquement à
la copie privée transmise à la restauration de `Task`. Cette normalisation est
éphémère et déterministe : elle ne stage, ne promeut et ne publie aucun
checkpoint contenant seulement une estimation différente sous le même résumé.
Une panne pré-terminale peut donc répéter la normalisation exacte. Cette couture
ne peut modifier ni identité, paramètres scientifiques, progression ou curseur
métier, et toute forme voisine est rejetée.

## Inventaire production et entrées runtime

`src/task_kinds.c::lardon3d_task_kind_registry_production()` enregistre les
14 kinds v1 du profil de production courant. La colonne « reprise » nomme le
reconstructeur du binding ; « callback » nomme l'entrée runtime privée dans le
même fichier. Le détail chiffré des capacités est centralisé dans l'[audit des
14 kinds](resource_governor.md#audit-des-14-kinds-de-production).

| Kind v1 | Source, reprise et callback | Réconciliation pré-admission courante |
| --- | --- | --- |
| `raw.develop` | `raw_development_task.cpp`; `lardon3d_raw_development_task_reconstruct`; `run` | Aucune |
| `photo_quality.triage` | `photo_quality_task.cpp`; `lardon3d_photo_quality_task_reconstruct`; `run` | Aucune |
| `acquisition_campaign.run` | `acquisition_campaign_task.cpp`; `lardon3d_acquisition_campaign_task_reconstruct`; `run` | Forme courante ou forme v22 exacte vérifiée contre la requête immuable → capacité courante en mémoire |
| `import.images` | `import_task.c`; `lardon3d_image_import_reconstruct`; `run_image_import` | Aucune |
| `features.extract` | `feature_task.c`; `lardon3d_feature_extract_reconstruct`; `run` | Formes CPU12/CPU1 historiques exactes → demande OpenCV portable ; runtime borné au compute-pool |
| `features.extract.sift` | `sift_task.c`; `lardon3d_sift_extract_reconstruct`; `run` | CPU12/CPU1 historiques exacts → demande OpenCV portable ; runtime borné au compute-pool |
| `features.extract.rootsift` | `sift_task.c`; `lardon3d_sift_extract_reconstruct`; `run` | Même réconciliation SIFT ; aucune voie GPU validée |
| `visual_index.update` | `visual_index_task.c`; `lardon3d_visual_index_update_reconstruct`; `run` | Formes CPU12/CPU1 historiques exactes → CPU/lot 1..16 |
| `candidate_pair.generate` | `candidate_pair_task.c`; `lardon3d_candidate_pair_generate_reconstruct`; `run` | Formes CPU12 et CPU1 historiques exactes → CPU/lot 1..64 |
| `matcher.run` | `matcher_task.c`; `lardon3d_matcher_task_reconstruct`; `run` | Signatures CPU/Vulkan historiques exactes → formes courantes en mémoire |
| `geometric_verifier.run` | `geometric_verifier_task.c`; `lardon3d_geometric_verifier_task_reconstruct`; `run` | Forme sérielle CPU1/batch8 exacte → CPU utile 1..8, lot 1..16 |
| `track_builder.run` | `track_builder_task.cpp`; `lardon3d_track_builder_task_reconstruct`; `run` | Aucune |
| `sparse_sfm.run` | `sparse_sfm_task.cpp`; `lardon3d_sparse_sfm_task_reconstruct`; `run` | Aucune |
| `incremental_reconstruction.run` | `incremental_reconstruction_task.cpp`; `lardon3d_incremental_reconstruction_task_reconstruct`; `run` | Aucune |

## Couture privée Compute Governor v2

**COMPUTE_GOVERNOR_V2 — PASS / FROZEN.** Le descriptor C public
reste limité à kind, version et reconstructeur. L'enveloppe de capacités est
intégrée sans changement d'ABI dans le `struct Lardon3DTask` opaque et les
coutures privées `src/task_internal.h` / `src/resource_governor_internal.h`.
Les coutures d'admission sont `src/task_queue.c::select_admissible()`,
`src/task.c::lardon3d_task_sequence_break()` et, côté Governor,
la sélection multi-capacité sur un snapshot unique. La normalisation historique
exacte reste dans `src/task_kind_registry.c::normalize_known_legacy_estimate()`.

Cette enveloppe n'est ni une identité scientifique, ni un nouveau payload
Project DB, ni un nouveau scheduler. Le Governor possède l'admission de tous
les kinds, y compris les formes entièrement fixes. Le contrat choisi est
immutable pendant une séquence et seule la suivante peut être adaptée. Une Task
sans alternative reçoit automatiquement une capacité égale à son estimation
durable. Le Governor conserve un état borné par kind/backend et un dernier
diagnostic ; ni l'enveloppe ni ce choix ne sont persistés.

La politique CPU hôte reste privée au Governor : masque permis, groupes
package/core/SMT, compute-pool et résultat d'application du worker Queue. Le
compute-pool borne l'admission de chaque kind. Feature/SIFT/RootSIFT utilisent
le maximum `int` positif comme borne de l'API OpenCV, puis consomment le compte
immutable réellement admis ; les CPU12 durables ne sont plus que des signatures
historiques exactes. Les kinds CPU1 justifiés restent fixes. Aucun ID CPU ou
choix d'affinité n'entre dans le descriptor, le checkpoint ou le Project DB.

Le feedback ne requalifie pas un succès de reprise en travail durable : les
kinds Feature/SIFT/RootSIFT comptent un item seulement après extraction et
publication durable propre. READY, collision `ALREADY_PRESENT` ou publication
incertaine compte zéro ; Visual Index compte pareillement zéro pour un segment
`PUBLISHED_NOT_DURABLE`.

L'état privé par kind/backend coordonne désormais une seule dimension d'essai.
Les CPU réductibles slow-startent par doubles successifs depuis 1, puis le
maximum exact de leur capacité, toujours bornés par le compute-pool. Après deux
observations de baseline, deux observations à au moins +5 % sont nécessaires
pour accepter le palier.
Une fois CPU stabilisé, seuls les kinds dont le callback consomme réellement
son lot peuvent ouvrir un essai de lot. `features.extract`, SIFT et RootSIFT
enregistrent une observation atomique réussie partagée entre Tasks ; Visual
Index, Candidate et Matcher enregistrent chaque séquence. Les autres formes ou
dimensions non adaptables restent égales à leur capacité fixe honnête.

## Persistance et legacy

Le checkpoint générique reste en version 1. Project Database v7 conserve le
kind/version ; les lignes migrées depuis v1 restent `NULL/NULL` et sont classées
`LEGACY_UNTYPED`. Un kind inconnu ou une version non supportée reste inspectable
mais inexécutable. Aucun type n'est inventé et aucun code n'est sélectionné par
adresse persistée.

## Statut

**IMPLEMENTED** — identité typée immutable, registry statique, lookup,
migration DB v1→v2, classification recovery et restauration explicite testée.

**IMPLEMENTED** — le descriptor production `import.images`, version 1, charge
le chemin source borné depuis la table dédiée et reconstruit callback et
userdata sans `AppState *` ancien.

**IMPLEMENTED** — `project_open()` utilise la registry production immutable
pour restaurer hors mutex DB et transférer chaque tâche acceptée à la queue.

**NOT_YET_WIRED** — réconciliation orpheline et dépendances/DAG. Les kinds
reconstructibles checkpointent déjà à leurs frontières métier ; la Registry ne
possède pas un timer autosave et ne doit pas devancer leurs curseurs durables.

**IMPLEMENTED** — `features.extract` version 1 reconstruit une extraction ORB
depuis `image_id` et ses paramètres bornés.

**IMPLEMENTED** — `visual_index.update`, version 1, recharge
`visual_index_id + after_feature_set_id` et reconstruit un contexte neuf.

**IMPLEMENTED** — `candidate_pair.generate`, version 1, recharge
`visual_index_id + after_feature_set_id + top_k + minimum_evidence_count
+ scanset_filter + exclude_same_asset` depuis `candidate_pair_generate_tasks`
et reconstruit un contexte boundé. La restauration reconnaît le snapshot
opérationnel sériel historique v1 exact et la forme CPU12 immédiatement
antérieure, puis les remplace éphémèrement par la demande courante CPU64 avec
8 Mio par item avant admission. Le checkpoint historique et le curseur typé
restent inchangés ; une forme voisine est rejetée.

**PASS / FROZEN — Compute Governor v2.** `matcher.run`, version 1,
recharge la configuration Matcher, l'identité Feature Set et le curseur
`after_candidate_pair_id`. Il traite une Candidate Pair atomique à la fois dans
des lots bornés à douze, checkpoint le curseur et repasse par le Governor entre
les lots. La table durable `matcher_tasks` est introduite par Project DB v11,
après le Match Result v10. Son reconstructeur accepte les formes courantes
CPU12/GPU0 et CPU1/GPU1/640 Kio à lot `1..12`, les signatures historiques
CPU8/GPU0 et Vulkan à lot maximal 8, puis les formes CPU12
pré-estimation-par-paire. La normalisation reste en mémoire. Une forme voisine
échoue au lieu de servir d'indice de backend ; le payload Project DB ne change
pas et ne persiste aucune identité matérielle. Les nouvelles Tasks ORB normales
ont une signature de classe `MIXED`, dont les autres champs restent une demande
de ressources réelle ; elle seule reconstruit la politique Governor `AUTO`.
Toutes les formes ORB de classe `CPU`, anciennes ou courantes, reconstruisent
un CPU fixe pour préserver les overrides explicites et une compatibilité sûre ;
une forme Vulkan restaurée reste fixe Vulkan. Un build portable reconstruit la
même politique `MIXED` mais n'expose que sa capacité CPU. Les snapshots tout à
zéro Candidate/SIFT/RootSIFT sont explicitement corrompus ; seules leurs
signatures historiques complètes exactes sont acceptées. Seule la forme AUTO
restaurée établit la disponibilité Vulkan partagée. Restaurer ensuite CPU,
Vulkan ou une signature historique fixe n'écrit rien dans cet état : la
co-restauration est indépendante de l'ordre. Aucun nouvel état de
backend n'est persisté.

Pour une Task AUTO, la Registry reconstruit aussi l'enveloppe privée Vulkan
CPU1/GPU1, lot opérationnel `1..8`, helpers 0 et inflight 1. La signature
durable historique reste à lot `1..12`; la signature 640 Kio
reste la forme depth-1 minimale et n'est pas mutée; l'admission normale facture
exactement 640 Kio une seule fois sur UMA. Le choix inflight est immutable dans
la séquence et ne devient ni payload, ni fingerprint, ni indice de reprise.
Vulkan explicite reste depth 1. La capacité privée de sûreté/benchmark peut
forcer deux slots et 1,25 Mio sans changer la reconstruction normale.
Le backend ne mappe pas le maximum de l'enveloppe à sa création : il retient
exactement un slot à depth 1 et deux seulement sous une séquence depth 2 admise,
puis libère le second avant l'admission suivante. La signature durable 640 Kio
reste donc inchangée sans sous-facturer une allocation depth 2 forcée.

L'A/B forcé ABBA a mesuré 54,661652238 paires/s à depth 1 et 55,797311953 à
depth 2, soit +2,077617 %, sous le deadband 5 %, avec digest identique, quatre
séquences de fallback local par exécution et zéro panne/discard. La Registry conserve donc
`DEPTH_MAX_VALIDATED_SAFETY=2` pour les seules coutures privées, mais la
capacité AUTO normale suit `DEPTH_MAX_USEFUL=1` : depth 2 est
**REJECTED_WITH_MEASURED_REASON**, sans nouvelle signature durable.

La télémétrie privée de `matcher.run` conserve les classes de fallback par
séquence et compte aussi les items exacts local-ineligible/backend-failure/other
après leur publication durable. Ce détail opérationnel n'ajoute aucun kind,
champ durable ou identité et empêche le regroupement batch de devenir un
comparateur scientifique. Le commit immédiat par item reste acquis si une
paire suivante avorte, tandis que le feedback de séquence n'est pas enregistré;
la déduplication actuelle vit seulement avec la Task reconstruite en mémoire.
Les logs batch 2/4 antérieurs à ce compteur restent préliminaires et prouvent
seulement l'invalidité du comparateur par séquences. Les huit runs item-valides
`forced-batch{2,4,8,12}-items{,-b}.stdout.jsonl` conservent chacun 4113 paires,
six items locaux, zéro panne/autre et le même digest. Les débits combinés sont
54,180767704, 66,094373197, 74,784998723 et 76,755814095 paires/s. Les gains
jusqu'à batch 8 dépassent 5 %, celui de 8 à 12 vaut seulement +2,635308425 % :
la Registry expose `BATCH_MAX_USEFUL=8` en AUTO normal et réserve batch 12 aux
preuves privées (`REJECTED_WITH_MEASURED_REASON`).
Le S21 final confirme l'enveloppe Registry en production : `matcher.run` v1
reste le même kind durable, AUTO choisit Vulkan pour 21 630 admissions et
termine 172 741/172 741 résultats à batch 8/inflight 1/helpers 0. Le passage
transitoire 8 → 1 → 2 → 4 → 8 ne modifie ni signature durable, ni
fingerprint, ni digest scientifique. Aucun backend ou champ persistant n'est
ajouté par cette adaptation.

La reconstruction AUTO ne sonde ni n'initialise Vulkan sur le thread
`project_open()`. Elle expose la capacité depuis les seules métadonnées runtime
build/backend/GPU ; le Governor possède le dimensionnement exact et
l'admission UMA sur son snapshot. Le premier begin appartient au worker Queue après son
affinité. La politique Mesa sûre est déjà établie avant les pthreads et
l'initialisation du driver ; aucun sweep/latch auxiliaire n'appartient donc au
contexte Task. Une paire localement inéligible n'initialise pas le backend. Une
panne réelle produit des paires CPU complètes et rend le backend indisponible
aux admissions AUTO suivantes sans réécrire le snapshot durable.

Le contrôle de benchmark `synchronous` du runner réel n'étend pas le descriptor
ni le reconstructeur. Il est compilé hors du binaire production, attaché
seulement au contexte éphémère d'une nouvelle Task et refusé par le runner si
une Task Matcher doit être reprise. La Registry continue donc à reconstruire
uniquement la politique AUTO/fixe déduite de la signature durable, jamais un
pipeline de benchmark.

**IMPLEMENTED** — ORB, SIFT et RootSIFT acceptent leurs formes CPU12/CPU1
historiques complètes et les normalisent vers la demande OpenCV portable
`INT_MAX`. Le Governor borne l'exécution au compute-pool ; les sorties testées
à 1/2/4/8/12 restent égales. Cette compatibilité opérationnelle n'altère ni
fingerprint, Feature Set, checkpoint durable, ni politique scientifique.

**IMPLEMENTED** — `geometric_verifier.run`, version 1, recharge la configuration
Fundamental immuable, en revalide le fingerprint et reprend `after_match_result_id`.
Project DB v13 ajoute uniquement `geometric_verifier_tasks`, car le checkpoint
générique v1 ne possède aucun payload propre au kind.

La forme historique série exacte (4 Mio fixes, CPU1, batch 1..8) est normalisée
en mémoire vers 8 Mio par item, CPU utile 8 et batch maximal 16. Cette évolution
ne touche ni fingerprint, GVR, ordre, curseur ni checkpoint historique ; une
forme voisine est refusée.

**IMPLEMENTED** — `track_builder.run`, version 1, reconstruit un scope explicite
depuis son payload Project DB v15 et son asset little-endian validé. Le callback
réutilise l'orchestration Gate C et le reconstructeur refuse toute corruption,
version, fingerprint, checksum, tri, unicité ou L3DTSIS1 incohérents.

**PASS / FROZEN** — `sparse_sfm.run`, version 1, recharge le
payload scientifique explicite Project DB v17, restaure l'estimation générique
persistée et rejoue D puis E depuis les références Track Set/calibration. Le
fingerprint F0 est recalculé ; le checkpoint générique v1 reste inchangé.

**PASS / FROZEN** — `incremental_reconstruction.run`,
version 1, recharge le payload Project DB v18 composé du prédécesseur, du Track
Set d'extension, du scope de calibration et du fingerprint H. La tâche atomique
recalcule depuis ces entrées après redémarrage, passe par la Queue et le
Governor avec son estimation H immuable, et ne persiste aucun état de solveur.
Elle n'ajoute ni DAG ni dépendance implicite.

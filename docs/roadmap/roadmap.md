# Roadmap canonique Lardon3D

Cette feuille de route décrit l'ordre de dépendance actuel. Les contrats détaillés
restent dans `docs/architecture/`; ce document indique ce qui est fermé, ce qui
vient immédiatement ensuite et ce qui demeure volontairement différé.

## Situation actuelle

Lardon3D possède une chaîne scientifique persistante allant des acquisitions
explicites aux frontières Sparse SfM et MVS validées. Les travaux volumineux
doivent rester incrémentaux, bornés, reprenables, durables et admis par l'unique
Resource Governor : aucune étape ne peut supposer qu'une campagne entière tient
en RAM ou termine dans une seule vie de processus.

### Fondations PASS / FROZEN

- Gates A–G Sparse SfM, dont F0, orchestration Task/Project DB et politique
  Governor : [Sparse SfM](../architecture/sparse_sfm.md) et
  [frontière ressource](../architecture/resource_boundary.md).
- Track Model / Track Builder v1 : [Track Model](../architecture/tracks.md) et
  [Track Builder](../architecture/track_builder.md).
- Phase H v1 d'enrichissement incrémental multi-snapshot :
  [Project DB](../architecture/project_database.md).
- MVS-M1, frontière OpenMVS v2.4.0, identité dense et export COLMAP/PLY borné :
  [pipeline de reconstruction](../architecture/reconstruction_pipeline.md).
- Task Runtime, checkpoints atomiques, Queue, Scheduler et Resource Governor :
  [Task](../architecture/task_system.md), [Queue](../architecture/task_queue.md)
  et [Governor](../architecture/resource_governor.md).
- Project DB v19 et S1–S3 Capture / Acquisition Ingestion : provenance
  Capture/Asset, import capture-safe, publication dérivée, développement RAW,
  siblings multi-source, évidence S3-D, orchestration S3-E et campagne bornée :
  [Project DB et ingestion](../architecture/project_database.md).

### S3 Capture / Acquisition Ingestion — PASS / FROZEN

```text
S3-A  Derived Image Publication             PASS / FROZEN
S3-B1 Deterministic RAW Development          PASS / FROZEN
S3-C  Multi-Source Capture Association       PASS / FROZEN
S3-D  Acquisition Pairing Evidence v1        PASS / FROZEN
S3-E  Multi-Source Ingestion v1              PASS / FROZEN
       Bounded Campaign Ingestion            PASS / FROZEN
Project DB                                   v19
```

```text
entrées d'acquisition explicites
→ publication SOURCE immuable
→ provenance Capture/Asset
→ métadonnées RAW/JPEG et évidence d'acquisition
→ propositions de campagne déterministes
→ regroupement fort ou explicitement confirmé
→ un Capture par observation physique résolue et siblings SOURCE
→ développement RAW déterministe si demandé
→ image_id scientifique immuable
→ pipeline aval existant
```

Les identités restent distinctes : `Capture != fichier`, `Capture != asset`,
`Capture != image_id`, `Capture != SHA-256` et `Capture != basename`.

### Validation réelle Sony A6000

Campagne `Photogrammetrie/2026-08-26_Baie_Moteur_A6000` :

- 953 ARW et 953 JPEG, soit 1906 sources ;
- métadonnées : ARW 953/953 OK, JPEG 953/953 OK ;
- JPEG Sony reconnus comme conteneurs MPF valides : APP2 `MPF\0`, images JPEG
  secondaires validées, remplissage inter-image/final nul, maximum privé huit ;
- `STRONG_GROUPS=0`, `CANDIDATE_PAIRS=953`, ambiguïtés 0, contradictions 0 ;
- plan identique après inversion de l'ordre des racines.

Le stem commun ne prouve jamais une acquisition. Les 953 propositions doivent
être confirmées et deviennent `CALLER_EXPLICIT`, jamais `STRONG`. La campagne
complète n'a pas encore été matérialisée.

### Limite de reprise DB v19

S3-E reprend une matérialisation connue par `resume_capture_id`. Une campagne
peut reconstruire son plan et conserver côté appelant le `capture_id` de chaque
groupe. Si le processus meurt après création du Capture mais avant rétention
durable de cet ID, DB v19 ne peut le redécouvrir sans inventer une identité
interdite. Il n'existe donc pas de garantie whole-campaign exactly-once. Une
future identité durable de requête/campagne pourra fermer cette fenêtre ; elle
ne sera jamais déduite d'un chemin, SHA, basename, timestamp, métadonnée ou
`image_id`.

## NEXT — exécution durable de campagne d'acquisition

La prochaine tranche architecturale unique est l'exécution opérationnelle du
plan S3 gelé en réutilisant Task, Scheduler et Governor existants :

1. identité durable de requête et payload Task borné ;
2. persistance/rechargement des confirmations, du curseur et des `capture_id` ;
3. matérialisation incrémentale d'un groupe S3-E par unité sûre ;
4. checkpoint après rétention durable, pause/reprise/annulation aux frontières ;
5. admission et budgets par l'unique Resource Governor ;
6. progression et échecs sans runtime, queue ou persistence parallèle.

Cette tranche décidera explicitement si une évolution additive après DB v19 est
nécessaire. Elle ne peut ni changer les identités S3 ni fusionner des Captures.

## NEXT REAL-DATA MILESTONE — A6000 ENGINE BAY END-TO-END

1. confirmer explicitement les 953 propositions ARW/JPEG ;
2. matérialiser progressivement environ 953 Captures ;
3. produire les représentations scientifiques par lots gouvernés ;
4. exécuter features, candidats, matching, vérification, tracks et Sparse SfM ;
5. évaluer la qualité de la reconstruction sparse ;
6. exécuter MVS/dense avec budgets et scratch contrôlés ;
7. publier durablement nuage dense et mesh ;
8. comparer le résultat aux tentatives photogrammétriques antérieures.

Ce milestone est une intégration réelle, pas une nouvelle série de micro-gates
S3. Les antécédents d'OOM/SIGSEGV OpenMVS, pression swap, grands intermédiaires
et temps longs imposent reprise, progression et contrôle de ressources.

## NEAR TERM

### Scratch SSD externe et swap optionnel

Matériel envisagé : SSD externe d'environ 500 Go dans un boîtier USB-C 10 Gb/s.
La capacité planifiée est : détecter un disque externe adapté, le présenter dans
la configuration TUI/projet et demander si ce projet doit l'utiliser.

Usages possibles : workspace/scratch, intermédiaires dense/mesh/texturing et,
sur activation explicite, swap de sécurité. Le contrat devra être possédé par
l'exécution Task, le Resource Governor et la politique temporaire du projet ;
il ne doit pas devenir un gestionnaire de stockage ad hoc.

- le scratch déplace les gros intermédiaires hors de la RAM et du disque système ;
- il peut permettre des jobs bornés plus grands ;
- SSD/swap ne sont jamais de la RAM ni une extension du budget scientifique ;
- latence et débit USB restent distincts de la mémoire ;
- l'utilisation reste optionnelle et Governor-controlled ;
- retrait, déconnexion, ownership, nettoyage et publication atomique exigent
  des sémantiques explicites ;
- aucun montage, formatage, `swapon` ou nettoyage destructif automatique n'est
  actuellement implémenté ou autorisé.

### Publication durable dense / mesh

MVS-M1 reste la frontière scientifique gelée. Les prochaines étapes portent sur
exécution, reprise, budgets mémoire/stockage et publication atomique :

```text
dense → mesh → refinement → texturing → consolidation → export
```

Le scratch devient particulièrement pertinent à cette frontière.

### Workflow TUI-first

La TUI exposera état projet, découverte de campagne, confirmation des candidats,
progression, pause/reprise/annulation disponible, Governor, choix du scratch et
étapes aval. ncurses reste au thread principal. Aucun projet GUI général n'est
introduit.

### Sources mixtes et multi-ScanSet

Sony A6000, Samsung S21 et futures sources peuvent varier en device, objectif,
résolution et format. Elles réutilisent le même modèle Capture/Asset/Image, sans
fork par caméra. Plusieurs ScanSets et représentations sélectionnées alimentent
l'enrichissement Phase H existant vers un modèle consolidé ; Phase H n'est pas
redéfini ici.

## LATER

### Ingestion vidéo et keyframes — PLANNED

Une vidéo sera une source d'acquisition portant sa propre provenance, pas un
pipeline scientifique parallèle :

```text
asset vidéo SOURCE
→ timeline et métadonnées déterministes
→ extraction bornée et déterministe de keyframes
→ filtres blur/qualité/redondance
→ candidats Capture
→ représentations image sélectionnées et immuables
→ pipeline scientifique Lardon3D existant
```

Le futur contrat devra lier chaque frame à l'asset vidéo source, définir une
identité d'extraction reproductible et retenir son timestamp. La sélection
combinera espacement temporel, netteté, rejet des frames redondantes, diversité
de mouvement/point de vue et couverture utile. Exécution, buffers et nombre de
frames resteront bornés ; reprise/checkpoint et admission appartiendront au
Task Runtime et au Resource Governor existants. Il n'existera **aucun pipeline
SfM séparé pour la vidéo** : les keyframes validées rejoignent les mêmes
Capture, images, features, matching, tracks, Sparse SfM et étapes aval que les
photos Sony, Samsung ou autres.

### Coverage Viewer et assistance à l'acquisition — PLANNED

Cette frontière passive de visualisation et d'aide analysera une reconstruction
publiée afin d'identifier où des photographies supplémentaires sont réellement
nécessaires. Ses entrées pourront inclure poses caméra, provenance Capture,
géométrie sparse/dense, nombre d'observations par track, densité de features,
qualité de reprojection, triangulation/parallaxe, visibilité de surface et
historique des ScanSets.

Overlays planifiés :

- positions et frustums des caméras ;
- régions bien couvertes et faiblement couvertes ;
- zones invisibles ou manquantes ;
- régions à faible diversité angulaire ou mauvaise parallaxe ;
- zones de reconstruction à faible confiance ;
- heatmap de couverture.

Les diagnostics devront produire des indications actionnables, par exemple :

```text
cette région demande plus de photographies
cette région demande un autre angle de vue
le nombre d'images suffit mais la parallaxe est insuffisante
cette cavité ou face est visible depuis trop peu de Captures
```

Toute recommandation dérivera d'évidence géométrique et de reconstruction,
jamais d'un nombre de fichiers, de basenames ou d'une heuristique d'identité.
Le workflow itératif visé est :

```text
ScanSet N
→ reconstruction
→ analyse de couverture
→ viewer des zones manquantes/faibles
→ acquisition supplémentaire
→ nouveau ScanSet
→ alignement/enrichissement Phase H
→ réévaluation de la couverture
```

Cette boucle reliera les acquisitions Sony A6000, Samsung/mobile, ScanSets
mixtes et futures keyframes vidéo sans fork par device. La sélection future de
keyframes pourra utiliser la couverture déjà reconstruite pour favoriser des
points de vue réellement complémentaires.

Lardon3D reste TUI-first. Le Coverage Viewer demeure un consommateur passif de
snapshots validés et une frontière séparée d'assistance ; il ne transforme pas
l'application principale en GUI et ne lit jamais les buffers workers mutables.

- **Maintenance projet** : vérification de provenance, assets orphelins,
  scrub/réconciliation et réclamation sûre du scratch. Aucun asset immuable
  partagé n'est supprimé silencieusement.
- **Exports/publication live** : snapshots validés et consommation sans accès
  aux buffers workers mutables.

## DEFERRED / OPTIONAL

- pools multiples CPU/GPU/IO, parallélisme inter-tâches et multi-GPU ;
- DAG général de dépendances et priorités complexes ;
- infrastructure backend générique et distribution de calcul ;
- ALIKED tant que provenance modèle, export ONNX et oracle upstream ne sont pas
  reproductibles.

Ces idées ne doivent pas devancer l'exécution durable de campagne ni créer un
second runtime, Governor ou système de persistance.

## Principes de séquencement

1. stabilité et intégrité scientifique avant débit ;
2. résultats atomiques et durables avant parallélisme ;
3. lots bornés et reprise avant taille de campagne ;
4. un Task Runtime, un Scheduler et un Resource Governor ;
5. scratch optionnel sans élargissement implicite des budgets RAM ;
6. TUI de contrôle avant visualisation riche ;
7. documentation canonique alignée sur le code validé.

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
→ filtres blur/netteté/redondance
→ diversité de mouvement et de point de vue
→ représentations frame sélectionnées / candidats Capture
→ pipeline scientifique Lardon3D existant
```

Le futur contrat devra lier chaque frame à l'asset vidéo source, définir une
identité d'extraction reproductible et retenir le timestamp exact de chaque
frame. Les sujets de conception incluent espacement temporel, netteté, rejet
des frames redondantes, diversité de mouvement/point de vue, traitement borné,
reprise et admission par le Resource Governor. L'analyse de couverture pourra
ultérieurement contribuer à la sélection :

```text
besoin de couverture actuel + trajectoire caméra / frames vidéo
→ retenir les frames apportant une information géométrique utile
```

Cette relation reste un sujet de recherche et d'ingénierie ; aucune politique
de sélection n'est gelée. Il n'existera **aucun second pipeline SfM propre à la
vidéo** : les keyframes validées rejoindront les mêmes Capture, provenance,
images, features, matching, tracks, Sparse SfM et étapes aval que les photos.

### Capture Guidance / Live Coverage — PLANNED, LONG TERME

Le but à long terme n'est pas seulement de reconstruire ce qui a été
photographié, mais de guider activement l'opérateur vers les photographies qui
manquent encore pour obtenir une reconstruction fiable. Cette capacité est
postérieure à une reconstruction suffisamment mature ; elle ne fait pas partie
de S3 et ne remplace pas la prochaine tranche d'exécution durable de campagne.

```text
photographies existantes
→ reconstruction Lardon3D
→ analyse de qualité de couverture
→ régions faibles ou manquantes
→ localisation de la caméra courante
→ projection dans la Live View
→ photographies supplémentaires par l'opérateur
→ ingestion / mise à jour de reconstruction
→ mise à jour de couverture ↺
```

#### Dépendances et niveaux de maturité

L'ordre conceptuel est :

```text
Sparse SfM
→ calibration et poses caméra
→ géométrie dense / mesh lorsque nécessaire
→ métriques de couverture
→ viewer et localisation live
→ Capture Guidance / Live Coverage
```

Une première analyse peut s'appuyer sur la géométrie sparse et les tracks ; un
mesh dense n'est donc pas une condition universelle. Les niveaux suivants sont
des paliers de roadmap, **pas de nouveaux Gates** :

1. **Offline Coverage Analysis** — calculer les régions faibles ou manquantes
   depuis une reconstruction existante.
2. **Coverage Viewer** — afficher géométrie, caméras, qualité de couverture et
   zones faibles.
3. **Suggested Supplementary Viewpoints** — associer une région faible à une
   direction ou un cône de points de vue suggéré.
4. **Live Camera Localization** — estimer la pose de la caméra courante par
   rapport à la reconstruction.
5. **Live Coverage Overlay** — reprojeter la couverture dans le flux vidéo.
6. **Closed Acquisition Loop** — guider, capturer, transférer, ingérer,
   reconstruire, réévaluer puis guider à nouveau.

#### Coverage Analysis — PLANNED

L'analyse estimera à quel point une région de surface ou de géométrie
reconstruite est soutenue par des observations photographiques. Ses entrées
potentielles comprennent :

- nombre de Captures observant la région, angle et diversité angulaire ;
- parallaxe disponible et diversité des points de vue ;
- résolution effective projetée sur la surface ;
- netteté, exposition et qualité d'image ;
- support features/tracks et qualité de reprojection/triangulation ;
- confiance de reconstruction, visibilité et occlusions ;
- provenance et historique des ScanSets.

Une expression telle que la suivante n'est qu'une intuition
**NON CONTRACTUELLE** :

```text
coverage_score = f(
    observation_count,
    angle_quality,
    parallax_quality,
    effective_resolution,
    sharpness,
    viewpoint_diversity,
    reconstruction_confidence
)
```

Les métriques exactes, poids, seuils et normalisations nécessitent une
validation expérimentale ultérieure. Aucun score scientifique n'est défini ou
gelé par cette roadmap.

#### Coverage Viewer — PLANNED

Le Coverage Viewer ne sera pas un simple viewer de mesh : il servira à examiner
la qualité d'acquisition et de reconstruction. Les overlays futurs pourront
montrer positions, directions et frustums des caméras, surfaces bien ou mal
couvertes, zones jamais vues, trous de reconstruction, faible nombre
d'observations, diversité angulaire ou parallaxe insuffisante, support
features/tracks faible, reconstruction peu fiable et contribution par ScanSet.

Une heatmap pourrait par exemple utiliser rouge pour une photographie
supplémentaire requise, orange pour un angle médiocre, violet pour une parallaxe
insuffisante, jaune pour un problème de qualité d'image et l'affichage normal
pour une couverture satisfaisante. Ces couleurs, catégories et significations
sont **des exemples seulement** : elles ne sont ni choisies ni gelées.

Lardon3D reste TUI-first. La TUI conserve le contrôle du projet, du workflow et
du runtime. Une frontière visuelle/acquisition séparée pourra posséder
l'affichage vidéo, le rendu points/mesh, les frustums, overlays et indications
live. Son architecture finale n'est pas définie ici ; elle devra préserver
l'isolation du viewer, consommateur de snapshots validés sans accès aux buffers
workers mutables.

#### Suggested Supplementary Viewpoints — PLANNED

Le résultat recherché dépasse la coloration d'une surface défaillante :

```text
région de surface faible + direction caméra / cône de points de vue suggéré
```

Il pourra exprimer « cette région demande des photographies supplémentaires »,
« photographier depuis une direction plus oblique », « le nombre d'images est
suffisant mais la diversité des points de vue ne l'est pas », « cette cavité
est vue par trop peu de Captures » ou « la géométrie d'acquisition fournit une
parallaxe insuffisante ». La recommandation devra dériver d'évidence géométrique
et de reconstruction, jamais du nombre de fichiers, de basenames similaires ou
d'heuristiques arbitraires déconnectées de la géométrie. L'algorithme
d'optimisation du point de vue n'est pas encore défini.

#### Live Camera Localization — PLANNED

Live Coverage dépendra de la localisation d'une vue nouvelle/live par rapport
à une reconstruction existante. Les prérequis probables incluent calibration
caméra, reconstruction sparse et points 3D existants, extraction de features
sur la frame live, correspondances 2D↔3D, estimation de pose calibrée, confiance
de pose et perte/réacquisition gracieuse du tracking. Cette capacité pourra
réutiliser la géométrie Sparse SfM, mais elle n'est ni conçue ni implémentée à
ce jour.

#### Live Coverage et boucle d'acquisition — PLANNED

La couche temps réel est distincte de l'analyse offline. Le déroulé cible est :

1. construire une reconstruction initiale et calculer l'évidence de couverture ;
2. recevoir sur le PC la Live View via une capture HDMI ;
3. estimer la pose de la caméra courante dans la reconstruction ;
4. projeter les régions 3D faibles/manquantes dans la frame vidéo ;
5. indiquer les acquisitions supplémentaires nécessaires pendant que
   l'opérateur déplace la caméra ;
6. déclencher une photographie et transférer RAW/JPEG par le chemin
   d'acquisition ;
7. faire entrer les nouveaux assets/Captures dans la provenance Lardon3D
   existante ;
8. mettre à jour reconstruction et couverture, puis retirer progressivement de
   l'overlay les régions corrigées.

L'expérience visée est conceptuellement : « les observations
photogrammétriques sont insuffisantes ici ; prendre une photographie
supplémentaire approximativement depuis cette direction ».

#### Frontière caméra HDMI / USB

Le Sony A6000 fournit un exemple concret de matériel cible, sans définir une
architecture scientifique propre à Sony :

```text
Sony A6000 ── HDMI → capture device → Live View ───────────┐
           └─ USB  → contrôle / shutter / RAW+JPEG ───────┤
                                                          ↓
                                                     Lardon3D sur PC
                    live camera pose + reconstruction/geometry/coverage
                                                          ↓
                                            reprojection et Live View overlay
```

Le calcul lourd, la reconstruction et le mesh restent sur le PC. La caméra est
principalement le capteur d'image, la source Live View et un dispositif
d'acquisition potentiellement contrôlable ; elle ne transporte ni n'exécute la
reconstruction. HDMI vise une Live View à faible latence. USB pourra selon les
capacités réelles de l'appareil fournir contrôle, déclenchement, métadonnées et
transfert RAW/JPEG. Tous les appareils ne partagent pas le même protocole : les
transports et contrôles spécifiques resteront à la frontière des adaptateurs
d'acquisition, hors de l'identité Capture gelée et du cœur scientifique.

Sony A6000, Samsung S21/mobile et futures caméras consommeront le modèle commun :

```text
Capture physique ↔ assets SOURCE ↔ représentations scientifiques sélectionnées
```

Capture Guidance consommera le modèle projet/reconstruction commun, sans fork
scientifique par device.

#### Boucle multi-ScanSet / Phase H

```text
ScanSet 1
→ reconstruction
→ analyse de couverture
→ zones faibles/manquantes
→ acquisition supplémentaire
→ ScanSet 2
→ ingestion / reconstruction
→ alignement et enrichissement Phase H
→ couverture mise à jour
→ répétition si nécessaire
```

Cette boucle est particulièrement importante quand un objet ne peut être
capturé en une seule passe. Elle réutilise Phase H v1 sans le redéfinir.

Pour une baie moteur, l'opérateur pourra à terme viser avec l'A6000 une bride,
une cavité ou une face mal observée mise en évidence dans la Live View, se
déplacer selon l'indication et ajouter une image avant ingestion et mise à jour
de la couverture. La valeur pratique est forte lorsque l'accès disparaîtra,
qu'un moteur ou composant doit être retiré, ou que le démontage modifiera la
scène : découvrir les photographies manquantes après coup pourrait être coûteux
ou impossible. Ce scénario motive la direction produit ; ce n'est pas un
contrat scientifique.

#### État futur et ordre de dépendance

Cette roadmap ne prétend implémenter aujourd'hui ni capture HDMI, contrôle USB,
pose live, projection de mesh, score ou heatmap de couverture, recommandation
automatique, ingestion vidéo, sélection de keyframes, ni reconstruction live
incrémentale. Ces capacités restent planifiées, ultérieures ou exploratoires.

L'ordre demeure sans ambiguïté :

```text
CURRENT NEXT
  exécution durable de campagne (Task / Queue / Scheduler / Governor,
  confirmations durables, progression et reprise)
→ INTÉGRATION RÉELLE
  campagnes A6000 + S21 → pipeline scientifique → dense/mesh → publication
→ LATER
  Coverage Analysis → Coverage Viewer → suggestions de points de vue
  → localisation live → intégration HDMI/USB → Capture Guidance / Live Coverage
```

Vidéo/keyframes pourra progresser en parallèle lors d'une phase ultérieure,
mais réutilisera toujours la même provenance Capture et le même pipeline
scientifique.

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

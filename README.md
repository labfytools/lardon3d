# Lardon3D

Moteur de reconstruction géométrique persistante et incrémentale, piloté par une TUI ncursesw.

## Vision

Lardon3D est un moteur de photogrammétrie Linux qui privilégie :

- **Stabilité** : aucune saturation du système hôte
- **Déterminisme** : résultats reproductibles et traçables
- **Faible consommation mémoire** : traitement par lots adaptatifs
- **Reprise après interruption** : résultats atomiques et persistants
- **Protection de la machine** : budgets bornés et respectueux
- **Traçabilité** : historique des opérations et métriques
- **Enrichissement progressif** : reconstruction incrémentale

Lardon3D ne vise pas simplement "dossier de photos → objet 3D", mais un ensemble
progressif d'observations et de contraintes donnant une reconstruction géométrique
persistante, enrichissable et versionnable.

## État actuel

### Briques validées

- **Project** : cycle de vie persistant, identité stable et Project Database ouverte
- **Import** : premier task kind de production, exécuté par la file générique en lots bornés et reprenables
- **ScanSet / Image Catalog v1** : acquisitions, images logiques, provenance et assets SHA-256 persistants et paginés
- **Capture / Asset Provenance v1** : Captures par ScanSet, associations source/dérivé
  et sélection explicite d'une image logique — PASS / FROZEN
- **Découverte et planification de campagne bornées** : racines explicites,
  plan metadata-only et exécution par groupes via S3-E — PASS / FROZEN ; campagne
  A6000 réelle validée sur 953 ARW + 953 JPEG MPF
- **Feature Store v1/v2** : ORB U8×32, SIFT/RootSIFT F32×128 et lecture typée bornée
- **Image View** : vues triées et filtrées pour la TUI
- **Task** : moteur de tâches avec pause/reprise, annulation et séquences
- **Task Checkpoint v1** : snapshot durable, fichier atomique et reprise sûre
- **Project Database v20** : fondations v19 préservées, avec persistance
  additive des tâches de campagne d'acquisition
- **Exécution durable de campagne d'acquisition** : tâche générique à requête
  typée immuable, confirmations `CALLER_EXPLICIT`, curseur et correspondance
  tâche/groupe→Capture persistants ; un groupe S3-E par séquence, reprise par
  la registry, la Queue et le Resource Governor existants
- **Sparse SfM Gates C/D/E** : géométrie calibrée, noyau incrémental et Bundle
  Adjustment final par composante, tous PASS / FROZEN
- **Sparse SfM Gate F** : orchestration durable, runtime gouverné et publication
  atomique, PASS / FROZEN
- **MVS-M1** : frontière externe OpenMVS v2.4.0
  `InterfaceCOLMAP`/`DensifyPointCloud`, export COLMAP déterministe (OpenCV
  undistortion, observations transformées et tracks réels), texte exporté en
  flux et tracks indexés sans rescanner quadratiquement les observations ; espace
  de travail privé neuf par invocation sous le staging appelant, sans réemploi ;
  identité dense liée à la reconstruction de base, au jeu source, au
  `calibration_scope_identity` historique, au binding numérique MVS, au backend
  et aux paramètres ; `L3DMDID2` v2 (220 octets) et binding `L3DMCAL1` v1 ; PLY
  OpenMVS binaire little-endian validé (en-tête <= 1 MiB en octets bruts,
  LF/CRLF acceptés, CR seul malformé rejeté, ligne <= 64 KiB), fusionné en
  mode 0 — PASS / FROZEN
- **Geometric Verification Model v1** : identité, masque d'inliers et modèle 3×3 persistants
- **Geometric Verifier v1** : Fundamental USAC/MAGSAC, reprise et lots resource-aware
- **Task Kind Registry** : identité métier durable et reconstruction runtime explicite
- **Recovery projet** : reprise automatique sélective et bornée des imports récupérables
- **Task Queue** : file FIFO avec sélection adaptative et backpressure
- **Hardware Profile** : détection des capacités matérielles
- **Resource Snapshot** : capture instantanée des ressources
- **Resource Governor** : arbitrage centralisé des budgets et réservations

### Intégration réelle validée

- **Intégration multi-campagne A6000 + S21 FE Engine Bay** : PASS — les plans
  réels A6000 (953 paires confirmées `CALLER_EXPLICIT`) et Samsung SM-G990B
  (3544 JPEG singleton) ont été validés dans deux ScanSets d'un même projet
  temporaire, avec exécution durable, Governor/Queue et reprise sans Capture
  dupliqué. La suite reste le pipeline scientifique aval, selon la
  [roadmap canonique](docs/roadmap/roadmap.md).

### Plus tard / différé

- publication durable dense/mesh et scratch SSD externe optionnel gouverné ;
- workflow TUI de confirmation/progression ;
- vidéo/keyframes et **Capture Guidance / Live Coverage** : analyse et viewer de
  couverture, suggestions de prises de vue puis assistance live, après
  reconstruction mature ;
- exports et publication live ;
- DAG général, pools multiples et parallélisme inter-tâches restent différés.

## Architecture

```text
TUI / Projet
    ↓
Scheduler
    ↓
Resource Governor
    ↓
Workers
    ↓
Résultats atomiques / persistants
    ↓
Viewer (consommation passive de snapshots)
```

### Invariants fondamentaux

- Aucun callback de tâche sans réservation active validée
- Le scheduler ne décide jamais des ressources
- Le Resource Governor est l'unique propriétaire des budgets
- ncurses appartient exclusivement au thread principal
- Les estimations de ressources sont immuables
- Les buffers et files sont strictement bornés

## Pipeline cible

```text
Acquisitions
→ catalogue
→ features
→ index visuel
→ paires candidates
→ matching
→ vérification géométrique
→ tracks / SfM
→ dense
→ mesh
→ consolidation
→ export
```

## Documentation

### Architecture
- [Vue d'ensemble](docs/architecture/overview.md)
- [Runtime](docs/architecture/runtime.md)
- [Système de tâches](docs/architecture/task_system.md)
- [Registry des types de tâches](docs/architecture/task_kind_registry.md)
- [File de tâches](docs/architecture/task_queue.md)
- [Resource Governor](docs/architecture/resource_governor.md)
- [Pipeline sensible aux ressources](docs/architecture/resource_aware_pipeline.md)
- [Intégration Scheduler ↔ Governor](docs/architecture/scheduler_resource_integration.md)
- [Pipeline de reconstruction](docs/architecture/reconstruction_pipeline.md)
- [Persistance](docs/architecture/persistence.md)
- [Base de données projet](docs/architecture/project_database.md)
- [Feature Store](docs/architecture/feature_store.md)
- [Precision Feature Pipeline v1A](docs/architecture/precision_feature_pipeline.md)
- [Visual Index](docs/architecture/visual_index.md)
- [Candidate Pair](docs/architecture/candidate_pair.md)
- [Match Result](docs/architecture/match_result.md)
- [Matcher](docs/architecture/matcher.md)
- [Geometric Verification](docs/architecture/geometric_verification.md)
- [Geometric Verifier](docs/architecture/geometric_verifier.md)
- [Track Model](docs/architecture/tracks.md)
- [Sparse SfM / Triangulation — Gate A](docs/architecture/sparse_sfm.md)
- [Backend Vulkan ORB](docs/architecture/vulkan_matcher.md)
- [Viewer](docs/architecture/viewer.md)
- [Resource Boundary — No New Resource Subsystem](docs/architecture/resource_boundary.md)
- [Revue des fondations](docs/architecture/foundation_review.md)

### Concepts
- [Scan Sets](docs/concepts/scan_sets.md)
- [Index visuel](docs/concepts/visual_index.md)
- [Matching et tracks](docs/concepts/matching_and_tracks.md)
- [Couches de reconstruction](docs/concepts/reconstruction_layers.md)
- [Contraintes géométriques](docs/concepts/geometric_constraints.md)

### Développement
- [Build](docs/development/build.md)
- [Tests](docs/development/testing.md)
- [Concurrence](docs/development/concurrency.md)
- [Profil de performance de la machine cible](docs/performance/target_hardware.md)

### Roadmap
- [Roadmap](docs/roadmap/roadmap.md)

## Build rapide

```sh
CC=clang meson setup build --wipe
meson compile -C build -j8
```

## Tests

```sh
meson test -C build --print-errorlogs
git diff --check
```

Pour les changements sensibles à la mémoire ou à la concurrence, ajouter ASan/UBSan et TSan.

## Statut

Lardon3D est en développement actif. La persistance des tâches, le catalogue,
le Feature Store multipasse, le Visual Index ORB, Candidate Pair Generator
Matcher v1, Geometric Verification Model v1 et Geometric Verifier Fundamental v1
sont implémentés. Le runtime Feature + Matcher + Verifier emploie des tâches durables,
de petits lots, le Resource Governor interactif et un hot path Vulkan ORB exact avec
fallback CPU. La feasibility Vulkan SIFT/RootSIFT a été rejetée ; ces deux matchers
restent sur OpenCV L2. Track Model/Builder, les primitives géométriques Gate C,
le noyau Sparse SfM incrémental Gate D et le Bundle Adjustment final Gate E sont
implémentés et validés. L'orchestration Sparse SfM Gate F est PASS / FROZEN ;
l'intégration Governor Gate G est **PASS / FROZEN**. MVS-M1 est **PASS / FROZEN** :
une frontière OpenMVS v2.4.0 externe et bornée, sans publication dense durable
ni MVS complet. Les sources sont liées par SHA-256
complet, borné à 1 GiB par fichier régulier (sans budget agrégé de dataset) ; les
octets source restent un binding distinct de l'identité dense. Celle-ci lie la
reconstruction de base, le jeu d'images source, le `calibration_scope_identity`
historique, le binding numérique de calibration MVS `L3DMCAL1` v1, le backend et
les paramètres dans `L3DMDID2` v2 (220 octets). Chaque appel utilise un espace de
travail privé neuf sous le staging appelant, sans réemploi d'une scène,
profondeur, cache ou sortie antérieure. Le DAG, le viewer et les autres étapes
denses restent des tickets séparés planifiés.
Le Resource Governor ne constitue pas un Resource System générique : voir la décision
d’architecture.

## Licence

Projet privé - Tous droits réservés.

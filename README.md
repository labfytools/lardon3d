# Lardon3D

Moteur de photogrammétrie générique, persistant, incrémental et sensible aux
ressources, piloté par une TUI ncursesw.

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
- **Task Checkpoint v1** : snapshot durable, protocole `.chk.next` → SQLite →
  `.chk` sous verrou par tâche, et reprise sûre
- **Project Database v23** : overlay optique additif au-dessus de la fondation
  v22 PASS / FROZEN ; profils de boîtier et d'objectif, configurations
  body+lens+focale, affectations campagne/Capture et calibrations exactement
  compatibles, sans inférence ni backfill — IMPLEMENTED / VALIDATED / REVIEWED
- **[Photo Quality Triage](docs/architecture/photo_quality_triage.md)** : métriques JPEG
- **[Calibration Bootstrap v1](docs/architecture/calibration_bootstrap.md)** :
  import borné d'une calibration optimisée et traçable avant le Sparse SfM à
  calibration connue — PASS / FROZEN ; ni
  auto-calibration interne ni EXIF comme source de calibration scientifique
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
- **Geometric Verification Model** : identité, masque d'inliers et modèle 3×3
  persistants pour les policies Verifier v1/v2 historiques et v3 courante
- **Geometric Verifier v3** : Fundamental USAC/MAGSAC, reprise et lots resource-aware
- **Internal Parallelism + Compute Resources v1** : parallélisme interne borné,
  sorties canoniques et admission Governor — PASS / FROZEN
- **Task Kind Registry** : identité métier durable et reconstruction runtime explicite
- **Recovery projet** : reprise automatique sélective et bornée des imports récupérables
- **Task Queue** : file FIFO avec sélection adaptative et backpressure
- **Hardware Profile** : détection des capacités matérielles
- **Resource Snapshot** : capture instantanée des ressources
- **Resource Governor** : arbitrage centralisé des budgets et réservations
- **TUI observatoire / centre de contrôle** : modèle de vue pur et borné,
  observation coalescée, progression durable/ETA honnête, écrans Tasks,
  Resources, Optique et SSD, avec ncurses exclusivement sur le thread principal
  — CURRENT / VALIDATED OPERATIONAL
- **Contrôleur SSD externe optionnel** : frontière physique UDisks2/GDBus,
  identité Drive+labels+UUIDs, drain sûr et capacités de contrôle exactes ; son
  état physique est enregistré auprès du Governor, seul orchestrateur des
  leases scratch de production — CURRENT / VALIDATED OPERATIONAL

### Intégration réelle validée

Sony A6000 et Samsung S21 FE sont des preuves de validation de la chaîne
générique. Ils ne définissent ni l'identité produit, ni un profil caméra
hardcodé, ni une limite de CPU ou de dataset.

- **Intégration multi-campagne A6000 + S21 FE Engine Bay** : PASS — les plans
  réels A6000 (953 paires confirmées `CALLER_EXPLICIT`) et Samsung SM-G990B
  (3544 JPEG singleton) ont été validés dans deux ScanSets d'un même projet
  temporaire, avec exécution durable, Governor/Queue et reprise sans Capture
  dupliqué. Les campagnes réelles actuellement évaluées sont
  `CALIBRATION_UNAVAILABLE`, donc le Sparse SfM réel est
  `BLOCKED_BY_KNOWN_CALIBRATION_DATA` : ce n'est ni un échec logiciel, ni un
  rejet de qualité, ni une autorisation d'importer une pseudo-calibration. La
  suite reste le pipeline scientifique aval, selon la
  [roadmap canonique](docs/roadmap/roadmap.md).

### Plus tard / différé

- publication durable dense/mesh et consommation Task explicite du scratch
  SSD optionnel ;
- vidéo/keyframes et **Capture Guidance / Live Coverage** : analyse et viewer de
  couverture, suggestions de prises de vue puis assistance live, après
  reconstruction mature ;
- exports et publication live ;
- DAG général, pools multiples et parallélisme inter-tâches restent différés.

## Architecture

```text
TUI / Projet
    ↓
Task Queue bornée (un worker, ordre/backpressure)
    ↓
Resource Governor (admission et réservation)
    ↓
Task callback admis (parallélisme interne borné si prouvé)
    ↓
Résultats atomiques / persistants
    ↓
Viewer (consommation passive de snapshots)
```

### Invariants fondamentaux

- Aucun callback de tâche sans réservation active validée
- La Queue/runtime ne décide jamais des ressources
- Le Resource Governor est l'unique propriétaire des budgets
- ncurses appartient exclusivement au thread principal
- Les estimations de ressources sont immuables
- Les buffers et files sont strictement bornés

### TUI opérationnelle

La TUI sépare le modèle de vue pur du rendu ncurses. Son observateur copie au
plus 129 entrées Queue (64 pending, une active, 64 historiques) et coalesce les
captures hôte autour d'une seconde ; aucun scan DB ou `/proc` volumineux n'a
lieu par frame. La progression scientifique exacte provient seulement des
compteurs durables typés. Le taux EWMA et l'ETA restent « calcul » jusqu'à deux
intervalles positifs, excluent le préfixe repris et deviennent explicitement
indéterminés, stalled ou throttled lorsque l'évidence l'exige.

Les tailles supportées sont 100×30 et plus en vue complète, 72×20 en compacte
de référence, et jusqu'au minimum 60×15 ; en dessous, seul « Terminal trop
petit » est affiché. Les couleurs ont toujours un équivalent textuel/bold/dim.
`F1` à `F7` ouvrent aide, projets, import, viewer futur, tâches, ressources et
optique. Le segment littéral `F10 SSD` reste visible à 60 colonnes et déclenche
uniquement l'action autorisée par le contrôleur. Pendant une saisie, seules
Enter, Échap et F10 sont actives ; pendant un import, seules `X` et F10 le sont,
et quitter/retour accueil sont explicitement désactivés.

Ouvrir, fermer ou changer de projet détruit et joint d'abord l'unique Queue,
callbacks terminaux inclus, puis ferme Project DB et recrée une Queue vide. Le
workflow optique utilise les alias metadata exacts, accepte normalement les
objectifs manuels sans EXIF, crée des profils/configurations immuables et exige
une affectation/sélection de calibration explicite et exactement compatible.

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
- [Parallélisme interne borné](docs/architecture/internal_parallelism.md)
- [Pipeline sensible aux ressources](docs/architecture/resource_aware_pipeline.md)
- [Intégration Queue/runtime ↔ Governor](docs/architecture/scheduler_resource_integration.md)
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
- [Audit global de maintenance — état consolidé](docs/architecture/global_maintenance_audit.md)
- [Revue historique des fondations](docs/architecture/foundation_review.md)

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
le Feature Store multipasse, le Visual Index ORB, Candidate Pair Generator,
Matcher v1, Geometric Verification Model et Geometric Verifier Fundamental v3
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
La fondation Project DB v22, `raw.develop` et Calibration Bootstrap v1 reste
**PASS / FROZEN**. La tête courante v23 ajoute seulement le contexte optique
générique : neuf tables, migration transactionnelle sans backfill, objectifs
manuels sans EXIF, configurations multiples par campagne et sélection de
calibration exactement compatible. Les migrations de copies réelles S21/A6000
ont conservé leurs lignes scientifiques et laissé l'overlay vide. Les campagnes
réelles S21 et A6000 Engine Bay sont
`CALIBRATION_UNAVAILABLE` par non-identifiabilité scientifique des données de
calibration connues ; le Sparse SfM réel reste
`BLOCKED_BY_KNOWN_CALIBRATION_DATA`, sans pseudo-calibration ni import inféré.
Le Resource Governor ne constitue pas un Resource System générique : il reste
l'unique propriétaire des budgets et le seul orchestrateur des leases scratch
de production. Le contrôleur SSD UDisks2 est une frontière physique séparée,
jamais un second scheduler ou Governor. Les quatorze Task kinds actuels ne
consomment encore aucun scratch : l'espace disponible est une capacité
observable, pas un usage fabriqué, et scratch/swap ne deviennent jamais de la
RAM. La TUI/F10 et cette intégration sont validées opérationnellement. L'audit
global est désormais `GLOBAL_MAINTENANCE_AUDIT=PASS/FROZEN` : après les builds
Clang frais portable/Vulkan, les suites normales 64/64 et 65/65, les sanitizers
applicables, TSan et les contrôles ABI, l'unique revue finale indépendante
GPT-5.6 SOL/ULTRA a conclu PASS sans finding bloquant. Elle a indépendamment
rejoué le build portable, la suite 64/64, une matrice focalisée 15/15 et les
76/76 probes strictes C17/C++17 couvrant 19 headers publics modifiés/nouveaux,
ainsi que l'ABI, les négatifs de seams production, le SHA du manifest GV et le
diff-check. Ce gel clôt la gate de maintenance ; il n'exécute pas à lui seul la
tranche scientifique suivante.

## Licence

Projet privé - Tous droits réservés.

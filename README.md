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

Lardon3D ne vise pas simplement "dossier de photos → objet 3D", mais "ensemble progressif d'observations et de contraintes → reconstruction géométrique persistante, enrichissable et versionnable".

## État actuel

### Briques implémentées (IMPLEMENTED)

- **Project** : cycle de vie persistant, identité stable et Project Database ouverte
- **Import** : import asynchrone et annulable d'images
- **Import Task** : wrapper asynchrone avec états et progression
- **Image Catalog** : indexage des métadonnées d'images
- **Image View** : vues triées et filtrées pour la TUI
- **Task** : moteur de tâches avec pause/reprise, annulation et séquences
- **Task Checkpoint v1** : snapshot durable, fichier atomique et reprise sûre
- **Project Database v1** : identité, tâches/checkpoints et inventaire d'artefacts SQLite
- **Task Queue** : file FIFO avec sélection adaptative et backpressure
- **Hardware Profile** : détection des capacités matérielles
- **Resource Snapshot** : capture instantanée des ressources
- **Resource Governor** : arbitrage centralisé des budgets et réservations

### Briques en cours de consolidation

- Intégration scheduler ↔ governor avec séquences adaptatives
- Documentation architecture

### Briques prévues (PLANNED)

- Reconstruction des callbacks métier et resoumission des tâches récupérables
- DAG de dépendances
- Pools de workers multiples (CPU/GPU/IO)
- Publication live validée
- Viewer intégré

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
- [File de tâches](docs/architecture/task_queue.md)
- [Resource Governor](docs/architecture/resource_governor.md)
- [Intégration Scheduler ↔ Governor](docs/architecture/scheduler_resource_integration.md)
- [Pipeline de reconstruction](docs/architecture/reconstruction_pipeline.md)
- [Persistance](docs/architecture/persistence.md)
- [Base de données projet](docs/architecture/project_database.md)
- [Viewer](docs/architecture/viewer.md)
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

Lardon3D est en développement actif. La phase de fondation est terminée. Les prochains tickets porteront sur la persistance, le DAG, les pools de workers et la publication live.

## Licence

Projet privé - Tous droits réservés.

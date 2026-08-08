# Vue d'ensemble de l'architecture Lardon3D

## Finalité et flux global

Lardon3D est un moteur de reconstruction géométrique persistante et incrémentale,
piloté par une TUI ncursesw. Le terminal reste le centre de contrôle : il gère
les projets, lance les opérations, présente leur progression et permet leur
annulation. Le viewer sera un composant graphique séparé mais intégré à
l'interface pour un usage confortable sur un seul écran.

```text
TUI / Projet
    ↓
Task
    ↓
Estimate
    ↓
Governor
    ↓
Reservation
    ↓
Scheduler
    ↓
Worker
    ↓
Résultat atomique
    ↓
Viewer live
```

## Composants actuels

### Project
Gestion persistante des projets : création, ouverture, fermeture, structure
de répertoires. Chaque projet regroupe configuration, images originales,
manifeste, résultats, exports et journaux.

**Statut :** IMPLEMENTED

### Import
Import asynchrone et annulable d'images dans un projet. Copie individuelle
des fichiers admissibles et maintenance d'un manifeste cohérent.

**Statut :** IMPLEMENTED

### Import Task
Premier type métier persistant. Il s'exécute par lots bornés dans le scheduler
générique, cible explicitement un ScanSet et peut être reconstruit puis repris.

**Statut :** IMPLEMENTED

### ScanSet et Image Catalog
Catalogue SQLite persistant séparant acquisition, image logique et asset
physique SHA-256. Les parcours persistants sont paginés ; l'ancien catalogue
mémoire depuis `manifest.tsv` reste une façade legacy pour la TUI.

**Statut :** IMPLEMENTED

### Image View
Vues triées et filtrées du catalogue pour la TUI. Ne modifie pas le
catalogue, le manifeste ou les images.

**Statut :** IMPLEMENTED

### Feature Store
Extraction ORB réelle par tâche persistante, Feature Sets logiques et assets
binaires content-addressed lisibles par plages bornées.

**Statut :** IMPLEMENTED

### Task
Moteur de tâches avec états, progression, pause/reprise coopérative,
annulation, checkpoints et estimations de ressources.

**Statut :** IMPLEMENTED

### Task Queue
File FIFO avec worker unique, sélection de la première tâche admissible,
backpressure et bornage du nombre de tâches en attente.

**Statut :** IMPLEMENTED

### Hardware Profile
Détection des capacités matérielles statiques : cœurs CPU, RAM, GPU/VRAM.

**Statut :** IMPLEMENTED

### Resource Snapshot
Capture instantanée des ressources disponibles : RAM libre, charge CPU,
VRAM disponible.

**Statut :** IMPLEMENTED

### Resource Governor
Arbitrage centralisé des budgets (RAM, GPU, CPU, IO), calcul de lots
adaptatifs, réservations opaques et historique borné de métriques.

**Statut :** IMPLEMENTED

## Résultats et publication live

Les traitements fonctionnent par séquences adaptatives : lire un lot borné,
calculer, écrire un résultat atomique, libérer la mémoire, puis traiter le
lot suivant. La stabilité du système hôte et la réactivité de la TUI ont
priorité sur le débit maximal.

Le viewer consomme des snapshots de résultats validés et publiés
atomiquement. Il ne lit jamais un fichier intermédiaire et ne partage pas
directement les buffers de travail d'un worker. Une interruption doit laisser
le dernier snapshot validé exploitable et permettre la reprise à une
frontière de séquence connue.

## Invariants fondamentaux

- Aucun callback de tâche n'est lancé sans réservation active validée.
- Le scheduler ne décide jamais des ressources.
- Le Resource Governor est l'unique propriétaire des budgets.
- Les réservations sont libérées exactement une fois.
- ncurses appartient exclusivement au thread principal.
- Les estimations de ressources sont immuables.
- Les buffers et files sont strictement bornés.

## Limites actuelles

- File à worker unique avec FIFO strict.
- Absence de DAG de dépendances.
- Absence de priorités.
- Absence de pools de workers multiples (CPU/GPU/IO).
- La TUI legacy ne sélectionne pas encore explicitement ses ScanSets.
- La réconciliation globale des assets/checkpoints orphelins n'est pas implémentée.
- Viewer et publication live non implémentés.

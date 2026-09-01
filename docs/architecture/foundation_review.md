# Revue historique des fondations Lardon3D

Ce document conserve l'état de la revue de fondation à son époque. Les limites
« absence de persistance », tombstones et tickets recommandés ci-dessous ne
décrivent plus l'état courant. Pour l'architecture active, voir
[Task](task_system.md), [Queue](task_queue.md),
[Resource Governor](resource_governor.md) et le
[registre de maintenance](global_maintenance_audit.md).

## Objectif

Documenter la revue technique de la phase de fondation : task, task_queue, hardware_profile, resource_snapshot, resource_governor, réservations et intégration au scheduler.

## Composants évalués

### Task
- Cycle de vie complet
- États et transitions
- Pause/reprise coopérative
- Annulation coopérative
- Checkpoints
- Estimations de ressources

### Task Queue
- File FIFO
- Sélection de la première tâche admissible
- Backpressure
- Bornage du pending_count
- Comportement WAIT

### Hardware Profile
- Détection des capacités matérielles
- CPU, RAM, GPU/VRAM

### Resource Snapshot
- Capture instantanée des ressources
- RAM libre, charge CPU, VRAM

### Resource Governor
- Arbitrage centralisé
- Calcul de lots adaptatifs
- Réservations opaques
- Historique borné

### Intégration Scheduler ↔ Governor
- Cycle d'exécution
- Admission
- Gestion des pauses
- Séquences adaptatives

## Invariants garantis

1. Intégrité des estimations (immuables)
2. Obligation de réservation active avant démarrage
3. Cohérence du contrat de lot transmis au callback
4. Gestion sécurisée de WAIT et des variables de condition
5. Libération unique des réservations
6. Protection mutex unique du gouverneur
7. Séparation stricte des rôles (scheduler ne décide pas des ressources)

## Limites connues lors de cette revue historique

- File à worker unique avec FIFO strict
- Absence de notification automatique de libération externe
- Accumulation de tombstones de réservations
- Absence de persistance
- Absence de DAG
- Absence de priorités
- Absence de pools de workers multiples

## Risques à surveiller

- Ordre de destruction des objets
- Concurrence sur la libération des réservations
- Récursivité/blocage par un callback détruisant sa propre tâche
- Bornage de la mémoire
- Indépendance de la publication atomique

## Feuille de route historique (désormais supersédée)

### Prochains tickets recommandés
1. Sélectionner une tâche admissible sans blocage par la tête de file ✓
2. Introduire le DAG et les dépendances
3. Persister les tâches et checkpoints de reprise
4. Orchestrer et mesurer les séquences adaptatives
5. Ajouter les pools bornés CPU, IO et GPU
6. Migrer l'import vers le scheduler générique
7. Ajouter la publication live validée, puis le viewer Vulkan séparé

## Validation

- Tests unitaires passés
- ASan/UBSan passés
- TSan passé
- git diff --check propre

## Statut : ARCHIVE D'ÉVIDENCE HISTORIQUE

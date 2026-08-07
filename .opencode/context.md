# Lardon3D – Persistent Context

## Project

Lardon3D est un moteur de photogrammétrie Linux écrit en C17.

Objectifs principaux :

- stabilité absolue ;
- faible consommation mémoire ;
- traitement par lots adaptatifs ;
- aucune saturation du système ;
- TUI toujours réactive ;
- architecture modulaire.

Le viewer Vulkan sera un processus séparé lisant uniquement des snapshots validés.

---

## Build

Compiler avec :

CC=clang meson setup build --wipe
meson compile -C build -j8

Validation minimale :

meson test -C build --print-errorlogs
git diff --check

Utiliser ASan/UBSan et TSan uniquement lorsque le ticket touche la mémoire ou la concurrence.

---

## Git

Interdictions :

- jamais git add -A
- jamais git commit
- jamais git push
- jamais modifier scan3d/
- jamais modifier tri_photos.py

Toujours fournir :

- liste exacte des fichiers du ticket
- validations exécutées
- limites restantes
- message de commit recommandé

---

## Architecture

Séparation stricte :

TUI
↓

Scheduler

↓

Governor

↓

Workers

↓

Résultats atomiques

Le thread ncurses ne fait jamais de travail métier.

Le scheduler ne décide jamais des ressources.

Le gouverneur est le seul propriétaire des budgets.

---

## Resource Governor

Estimate

↓

Reservation

↓

Scheduler

↓

Worker

Les réservations sont obligatoires avant toute exécution.

RAM iGPU partagée = RAM système.

La zram n'est jamais un budget de travail.

---

## Coding rules

C17 uniquement.

Pas de variables globales d'état.

Pas de system().

Pas de popen().

Pas d'allocation infinie.

Toujours des buffers bornés.

Sorties atomiques.

Rollback complet en cas d'échec.

---

## Current architecture

Modules disponibles :

- project
- import
- import_task
- image_catalog
- image_view
- task
- task_queue
- hardware_profile
- resource_snapshot
- resource_governor

---

## Active workflow

1. explorer uniquement les fichiers utiles

2. architect uniquement si nécessaire

3. un seul agent d'écriture

4. tests

5. revue

6. handoff

Ne jamais relire tout le dépôt.

Ne jamais faire de glob "*".

Ne jamais parcourir tous les fichiers.

---

## Models

Primary orchestration

MiMo V2.5 Free

Implementation

DeepSeek V4 Flash Free

Backup implementation

Nemotron 3 Ultra Free

Exploration

North Mini Code Free

Architecture / Review / Concurrency

Nemotron 3 Ultra Free

Documentation

Ling 3.0 Flash Free

Aucun modèle payant.

---

## If DeepSeek fails

Reprendre depuis :

.opencode/work/current_ticket.md

Ne jamais refaire :

- exploration
- architecture
- revue

si le handoff contient déjà ces informations.

---

## Project roadmap

Fondations terminées :

- TUI modulaire
- projets persistants
- import asynchrone
- catalogue d'images
- image_view
- moteur de tâches
- scheduler FIFO
- gouverneur de ressources
- réservations
- intégration scheduler ↔ governor
- sélection de la première tâche admissible
- documentation d'architecture

Ordre obligatoire des prochains tickets :

1. séquences de traitement adaptatives pilotées par le Resource Governor
2. contre-pression (backpressure) et bornage des files
3. checkpoints et persistance des tâches
4. DAG minimal de dépendances
5. priorités
6. pools CPU / GPU / IO
7. migration de l'import vers le scheduler générique
8. publication incrémentale
9. viewer Vulkan

Ne jamais proposer un ticket déjà terminé.

Ne jamais sauter une étape de cette feuille de route sans justification explicite.

---

## Mission

Toujours privilégier :

- stabilité
- simplicité
- déterminisme
- faible mémoire
- traitements séquencés
- protection du système

avant les performances brutes.

État actuel important :

- Le scheduler sait déjà sauter une tâche en WAIT en tête de file.
- Il sélectionne la première tâche admissible dans l’ordre FIFO.
- Ce ticket est terminé et ne doit pas être reproposé.

---

## Revue de concurrence obligatoire

Tout ticket touchant `task`, `task_queue`, le scheduler,
`resource_governor`, pthread, mutex, variables de condition, pause, reprise,
annulation, réservations ou états partagés exige `lardon-concurrency`.

Cette règle s’applique même si aucun nouveau thread n’est créé.

---

## Design philosophy

Chaque étape du pipeline doit être :

- indépendante ;
- interruptible ;
- reprenable ;
- observable ;
- testable ;
- déterministe.

Chaque tâche doit pouvoir être découpée en petits lots afin de :

- limiter l'utilisation mémoire ;
- garder une TUI fluide ;
- permettre une adaptation dynamique des ressources ;
- éviter toute saturation du système.

La stabilité du système hôte est prioritaire sur les performances brutes.

---

## Long-term objective

Lardon3D doit devenir un moteur de photogrammétrie entièrement piloté par un
scheduler adaptatif.

Le scheduler décide uniquement :

- quand lancer une étape ;
- avec quel lot ;
- dans quel ordre.

Le Resource Governor décide uniquement :

- si les ressources permettent l'exécution ;
- la taille optimale des lots ;
- quand différer une tâche.

Le viewer Vulkan ne participe jamais aux calculs.

Il affiche uniquement des snapshots validés produits par le pipeline.

L'utilisateur doit voir le résultat apparaître progressivement pendant les
calculs, sans attendre la fin complète de la reconstruction.

Toutes les étapes doivent pouvoir être interrompues puis reprises sans perdre
les résultats déjà validés.

---

## Optimisation

Privilégier toujours :

- plusieurs petits traitements ;
- plusieurs petites allocations ;
- plusieurs petites copies ;

plutôt qu'une grosse opération monolithique.

Lardon3D doit être optimisé pour des machines disposant de peu de mémoire
(16 à 32 Go) et d'un iGPU partagé.

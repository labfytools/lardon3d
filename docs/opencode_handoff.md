# Transmission du développement à OpenCode

## 1. Vision du projet

Lardon3D doit devenir un moteur complet de photogrammétrie Linux, piloté depuis
une TUI ncursesw. Le terminal reste le centre de contrôle pendant les calculs,
y compris ceux qui dureront plusieurs heures. La priorité est la robustesse :
maîtrise de la mémoire, reprise après interruption, résultats atomiques et
réactivité permanente du système hôte.

Le logiciel traite les données par petits lots : lecture, calcul, validation et
écriture, puis libération avant le lot suivant. Il préfère une progression plus
lente mais bornée à une opération monolithique susceptible de saturer la RAM,
le GPU intégré ou les entrées-sorties.

Lardon3D n'est pas un frontend OpenMVS. Son architecture, son modèle de projet,
son ordonnanceur, son gouverneur de ressources, ses formats de résultats et sa
reprise doivent lui appartenir. Un moteur externe éventuel serait un outil
encapsulé par une étape, jamais le propriétaire du pipeline ni de l'état.

## 2. Architecture actuelle

```text
TUI
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
```

La TUI gère ncurses, les entrées, les écrans et les snapshots destinés au
layout. Le layout dessine uniquement. Les modules métier ne dépendent pas de
ncurses et aucun worker ne l'appelle.

`Task` fournit état, progression, pause, reprise, annulation coopérative et
callback. Chaque tâche possède une `Lardon3DResourceEstimate` immuable. Le
profil matériel décrit les capacités stables ; un `ResourceSnapshot` mesure la
disponibilité dynamique. Le `ResourceGovernor` est l'unique arbitre des budgets
RAM, GPU, CPU et IO. Il répond `START`, `WAIT`, `REDUCE_BATCH` ou `REJECT` et
crée atomiquement une réservation opaque pour toute admission.

La `TaskQueue` actuelle est FIFO avec un worker. Elle demande une réservation
juste avant l'exécution, transmet une copie du contrat au callback et libère la
réservation après succès, échec ou annulation. En `WAIT`, elle dort sur une
condition variable jusqu'à notification. Une tâche en pause conserve son
contrat dans cette version.

## 3. Invariants

- Aucun callback de tâche sans estimation et réservation active validée.
- Le gouverneur décide des ressources ; le scheduler exécute le contrat sans le
  réinterpréter.
- Aucune variable globale d'état ni singleton.
- ncurses appartient exclusivement au thread principal.
- TUI, dessin et logique métier restent séparés.
- Les objets aux durées de vie complexes sont opaques et nettoyés explicitement.
- Une réservation est libérée exactement une fois ; la double libération est
  refusée sans modifier les budgets.
- Les budgets et calculs de lots contrôlent les dépassements d'entiers.
- Les sorties structurantes sont atomiques ; un rollback ne retire que ce que
  l'opération courante vient de créer.
- Les fichiers utilisateur antérieurs ne sont jamais écrasés silencieusement.
- La stabilité du système et la réactivité de la TUI priment sur le débit.
- Aucun traitement lourd monolithique : préférer des séquences et lots bornés.
- La mémoire d'un iGPU partagé est comptée dans le budget RAM système.
- Le swap et la zram sont des filets de sécurité, pas de la mémoire de travail.
- Une interruption doit préserver le dernier état validé et permettre la
  reprise à une frontière connue.
- Le viewer futur reste séparé, non bloquant et lecteur de snapshots validés.

## 4. État actuel

### Terminé

- TUI modulaire avec écrans Accueil, Projets, Import, Viewer, Aide, Tâches et
  Ressources.
- État applicatif central, projets persistants et configuration de leur racine.
- Import sûr, asynchrone et annulable vers `images/originals`, avec manifeste
  TSV atomique et rollback cohérent.
- Catalogue vérifié, navigation, tri et filtre en mémoire.
- Moteur générique de tâches et file FIFO à un worker.
- Détection du profil matériel et capture des snapshots Linux.
- Gouverneur thread-safe, estimations, lots adaptatifs et réservations opaques.
- Intégration scheduler/gouverneur avec réservation obligatoire avant callback.
- Tests normaux, ASan/UBSan et TSan des fondations.

### En cours

- Consolidation de la documentation et transfert vers OpenCode.
- Les contrats de lot existent, mais leur enchaînement en séquences complètes
  n'est pas encore orchestré.

### Non commencé

- Étapes de photogrammétrie, pipeline et cache de calcul.
- DAG, dépendances, priorités et scheduler multi-worker.
- Persistance des tâches et checkpoints après crash.
- Migration de l'import vers le scheduler générique.
- Publication live des résultats et viewer Vulkan.

## 5. Dette technique

- Le FIFO strict bloque toute la file lorsqu'une tâche en tête reçoit `WAIT`.
- La file possède un seul worker.
- Une libération externe exige un appel explicite à
  `lardon3d_task_queue_resources_changed()`.
- Les tombstones des réservations libérées restent alloués jusqu'à la
  destruction du gouverneur.
- Une erreur de capture des ressources échoue la tâche sans distinguer une
  panne transitoire.
- La destruction concurrente du gouverneur ou de la file avec leurs API actives
  n'est pas supportée ; la file doit être détruite avant le gouverneur.
- Les files ne possèdent pas encore de capacité maximale ni de contre-pression.
- Les tâches et checkpoints ne survivent pas au processus.
- L'import conserve son système de thread spécialisé.
- `meson.build` référence deux fois `resource_snapshot.c` et
  `resource_governor.c` dans la cible principale ; Meson le tolère, mais cette
  duplication déclarative devra être nettoyée séparément.

## 6. Ordre recommandé

1. Sélectionner une tâche admissible sans blocage par la tête de file, afin que
   `WAIT` n'immobilise pas des travaux compatibles avec les budgets restants.
2. Définir résultats atomiques, identifiants, métadonnées de validation et
   frontières de reprise avant de produire des calculs coûteux.
3. Ajouter le DAG et les dépendances pour représenter explicitement le pipeline.
4. Persister tâches et checkpoints afin de rendre la reprise réelle.
5. Orchestrer des séquences adaptatives mesurées, une réservation par lot.
6. Borner les files, ajouter la contre-pression, puis introduire les pools CPU,
   IO et GPU sans déplacer l'arbitrage hors du gouverneur.
7. Migrer l'import vers cette infrastructure validée.
8. Publier des snapshots live atomiques avant de commencer le viewer Vulkan.

Cet ordre évite de paralléliser ou visualiser des résultats dont le cycle de
vie, la validation et la reprise ne seraient pas encore définis.

## 7. Optimisations futures

Les éléments suivants sont prévus mais ne sont pas implémentés : DAG de
dépendances, scheduler sélectionnant intelligemment les tâches admissibles,
séquences et lots adaptatifs, apprentissage des tailles de lots à partir des
mesures, pipeline photogrammétrique, cache, reprise après crash, publication
live et viewer Vulkan. Le viewer devra être séparé de la TUI, s'afficher sur le
workspace 8 et consommer uniquement des snapshots validés.

Aucune de ces évolutions ne doit contourner les estimations et réservations ni
introduire des files ou allocations non bornées.

## 8. Profil matériel cible

Lardon3D détecte la machine au démarrage et observe ensuite ses ressources. Il
ne doit contenir aucune constante de dimensionnement propre à un processeur, un
volume de RAM ou un GPU particulier. Les valeurs matérielles connues servent à
établir des plafonds ; les snapshots dynamiques et réservations déterminent ce
qui peut réellement démarrer.

L'optimisation CPU, RAM, GPU et IO vise une utilisation soutenue mais sûre. Une
marge reste disponible pour Linux, la TUI et les autres applications. Les iGPU
partagent la RAM et doivent être comptabilisés comme tels. Le swap et la zram
signalent une pression dangereuse, jamais une capacité normale supplémentaire.

## 9. Principes d'évolution

- Lire `AGENTS.md` et toute la documentation d'architecture avant de modifier
  les fondations.
- Étendre les abstractions validées au lieu de les réécrire.
- Ne jamais contourner le gouverneur ni fabriquer un contrat d'exécution.
- Maintenir la propriété explicite des tâches, réservations, threads, mutex,
  conditions, descripteurs et allocations.
- Préférer plusieurs petits lots validés à une grande opération.
- Borner mémoire, files, parallélisme et IO avant toute optimisation de débit.
- Préserver l'annulation coopérative, les rollbacks ciblés et les écritures
  atomiques.
- Ajouter les tests de durée de vie et concurrence avec chaque évolution.
- Valider avec Clang, Meson, tests, `git diff --check`, ASan/UBSan et TSan selon
  le risque.
- Ne jamais modifier ni ajouter `scan3d/` ou `scan3d/tri_photos.py`.

## 10. Prompt OpenCode

```text
Tu reprends le développement de Lardon3D, un moteur complet de photogrammétrie
Linux en C17 piloté par une TUI ncursesw.

Avant toute modification :
- lis README.md, AGENTS.md, docs/opencode_handoff.md et tous les documents de
  docs/architecture/ ;
- inspecte entièrement le dépôt, l'état Git, les headers publics, les sources,
  les tests et meson.build ;
- comprends les propriétés, durées de vie et frontières de threads existantes ;
- préserve les changements utilisateur et ne touche jamais à scan3d/.

Respecte impérativement tous les invariants documentés : séparation TUI/métier/
layout, ncurses uniquement sur le thread principal, estimations immuables,
gouverneur seul arbitre des ressources, réservation active avant tout callback,
budgets et files bornés, traitement par petits lots, sorties atomiques,
rollback ciblé, reprise et stabilité du système prioritaire.

Ne réécris pas les fondations validées et ne contourne jamais le gouverneur.
Poursuis les développements dans l'ordre recommandé par ce document, un ticket
à la fois, sans ajouter prématurément DAG, pools, photogrammétrie ou Vulkan.
Exécute les validations prescrites par AGENTS.md et rapporte honnêtement leurs
résultats ainsi que la liste exacte des fichiers du ticket.
```

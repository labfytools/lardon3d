# Exécution et runtime Lardon3D

## Modèle d'exécution

### Threads
- Thread principal : TUI ncursesw (exclusif)
- Thread worker : exécution des tâches métier

### Synchronisation
- Mutex pour les accès partagés
- Variables de condition pour la coordination
- Atomicité des opérations critiques

## Cycle de vie d'une tâche

```text
1. Création (PENDING)
2. Soumission à la file
3. Sélection par le scheduler
4. Réservation obligatoire
5. Exécution (RUNNING)
   - Pause/reprise coopérative
   - Annulation coopérative
   - Séquences adaptatives
6. Complétion (COMPLETED) ou Échec (FAILED)
7. Nettoyage des ressources
```

## Synchronisation

### Mutex
- Protection des données partagées
- Accès exclusif aux ressources critiques

### Variables de condition
- Coordination entre threads
- Notification de changement d'état
- Attente passive (pas de polling)

### Atomicité
- Opérations indivisibles
- État cohérent garanti

## Gestion des erreurs

### Rollback
- Retour à l'état précédent en cas d'échec
- Nettoyage complet des ressources
- Aucun état partiellement appliqué

### Récupération
- Reprise à la dernière frontière connue
- Ignorance des artefacts partiels
- Validation avant publication

## Limites actuelles

- Worker unique (pas de pools multiples)
- Pas de parallélisme inter-tâches
- Checkpoints isolés disponibles mais pas encore orchestrés au démarrage

## Reprise durable

Un snapshot ne conserve que l'état logique d'une tâche. `RUNNING` et `PAUSED`
sont normalisés vers `PENDING`; aucun worker, callback brut, pointeur, contrat
ou réservation n'est restauré. Le propriétaire fournit un nouveau callback et
resoumet la tâche. Les états terminaux sont conservés.

`started_at` désigne le début de la tentative d'exécution courante, pas le
premier démarrage historique. Un checkpoint `RUNNING` restauré en `PENDING`
conserve temporairement l'horodatage de la tentative interrompue pour
l'observation ; lors de `lardon3d_task_start()`, `started_at` est remplacé par le
nouveau démarrage et `finished_at` est remis à zéro. `finished_at` n'est fixé
qu'à la terminaison de cette tentative.

**IMPLEMENTED** — snapshot, codec v1 et restauration isolée.

**NOT_YET_WIRED** — sauvegarde périodique, reconstruction métier des tâches et
resoumission automatique.

**PLANNED** — reprise globale du scheduler via la Project Database.

## Accès Project Database

**IMPLEMENTED** — une connexion SQLite opaque sérialisée par mutex interne ;
les opérations multi-tables sont transactionnelles et bornées.

**IMPLEMENTED** — le cycle de vie projet ouvre/crée `project.db`, vérifie
l'identité et ferme la connexion. L'application arrête la task queue avant la
fermeture finale du projet.

**NOT_YET_WIRED** — aucune reconstruction de callback/userdata ni resoumission
automatique. Une future resoumission devra aussi permettre à la queue de
conserver l'identifiant stable restauré au lieu d'en assigner un nouveau.

## Invariants

- ncurses appartient exclusivement au thread principal
- Aucune tâche ne démarre sans réservation active
- Les réservations sont libérées exactement une fois
- Les buffers sont strictement bornés

## Statut : DOCUMENTATION DE L'IMPLÉMENTATION ACTUELLE

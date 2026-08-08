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
- Pas de persistance des états

## Invariants

- ncurses appartient exclusivement au thread principal
- Aucune tâche ne démarre sans réservation active
- Les réservations sont libérées exactement une fois
- Les buffers sont strictement bornés

## Statut : DOCUMENTATION DE L'IMPLÉMENTATION ACTUELLE
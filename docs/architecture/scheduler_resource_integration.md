# Intégration Scheduler ↔ Resource Governor

## Responsabilité

Documenter l'intégration architecturale entre le scheduler de tâches et le Resource Governor, incluant les frontières de responsabilités, le cycle de vie des réservations et le comportement en cas de pause.

## Frontières de responsabilités

### Scheduler
- Ordonnancement FIFO
- Exécution des callbacks
- Gestion des états de tâche
- Sélection de la première tâche admissible

### Resource Governor
- Arbitrage des budgets (RAM, GPU, CPU, IO)
- Calcul des lots adaptatifs
- Réservations opaques
- Historique de métriques

## Cycle d'exécution

```text
1. Task → Estimate (estimation des ressources)
2. Scheduler → Governor → decide() (décision d'admission)
3. Governor → Reservation (réservation opaque)
4. Scheduler → Worker (exécution)
5. Worker → Governor → record_batch() (métriques)
6. Governor → release() (libération)
```

## Admission

### Réponses du gouverneur
- `ADMIT` : la tâche peut démarrer
- `WAIT` : la tâche doit attendre (pas de ressources)
- `REDUCE_BATCH` : réduire la taille du lot
- `REJECT` : rejeter la tâche

### Comportement en cas de WAIT
- Le scheduler saute la tâche en tête de file
- Il évalue la tâche suivante
- Pas de blocage de la file

## Gestion des pauses

### Comportement actuel
- Une tâche en pause conserve sa réservation
- Évite de perdre son contrat au profit d'un autre travail
- Permet la reprise avec le même lot

### Règle
- La réservation est conservée pendant la pause
- La libération n'a lieu qu'à la fin de l'exécution

## Séquences adaptatives

### Mécanisme
- `lardon3d_task_sequence_break()` permet de libérer la réservation courante
- Capturer un nouvel instantané de ressources
- Obtenir un contrat actualisé
- Reprendre le callback en conservant la progression

### Avantages
- Adaptation dynamique des lots en cours d'exécution
- Réponse aux changements de ressources
- Optimisation de l'utilisation mémoire

## Invariants

1. Aucune tâche ne passe de PENDING à RUNNING sans réservation active
2. Le scheduler ne prend jamais de décision sur les ressources
3. La libération des réservations s'effectue exactement une fois par cycle/séquence
4. Les atomicités sont garanties sous mutex

## Limites actuelles

- Worker unique (pas de pools multiples)
- Pas de DAG de dépendances
- Pas de priorités
- Pas de notification automatique de libération externe

## Statut : DOCUMENTATION DE L'IMPLÉMENTATION ACTUELLE

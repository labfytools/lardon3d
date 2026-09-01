# Intégration Task Queue ↔ Resource Governor

## Responsabilité

Documenter l'intégration architecturale entre le runtime/Task Queue et le
Resource Governor, incluant les frontières de responsabilités, le cycle de vie
des réservations et le comportement en cas de pause. Le nom historique de ce
fichier ne désigne pas un scheduler distinct.

## Frontières de responsabilités

### Task Queue / runtime
- Ordonnancement FIFO stable avec bypass des seuls `WAIT`
- Exécution des callbacks
- Gestion des états de tâche
- Sélection de la première tâche admissible

### Resource Governor
- Arbitrage des budgets (RAM, GPU, CPU, IO)
- Calcul des lots adaptatifs
- Réservations opaques
- Historique de métriques
- Registre borné de l'état SSD physique et orchestration des leases scratch de
  production, sans transformer cet espace en RAM

## Cycle d'exécution

```text
1. Task → Estimate (estimation des ressources)
2. Queue → Governor → decide() (décision d'admission)
3. Governor → Reservation (réservation opaque)
4. Queue → Worker (exécution)
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
- La Queue saute la tâche en tête de file seulement lorsqu'elle est en `WAIT`
- Il évalue la tâche suivante
- Pas de blocage de la file
- Si aucune tâche n'est admissible mais qu'un `WAIT` PENDING subsiste, le worker
  effectue une attente temporisée d'au plus 500 ms, puis rescane normalement la
  file avec de nouveaux snapshots. Tout signal explicite le réveille plus tôt.
- Cette attente appartient au worker existant : aucun thread de monitoring,
  nouveau scheduler ou nouveau sous-système n'est créé.

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
- L'attente d'admission à cette frontière conserve son polling existant de
  50 ms ; elle est distincte des 500 ms de réévaluation d'une tâche initialement
  PENDING.

### Avantages
- Adaptation dynamique des lots en cours d'exécution
- Réponse aux changements de ressources
- Optimisation de l'utilisation mémoire

## Invariants

1. Aucune tâche ne passe de PENDING à RUNNING sans réservation active
2. La Queue ne prend jamais de décision sur les ressources
3. La libération des réservations s'effectue exactement une fois par cycle/séquence
4. Les atomicités sont garanties sous mutex

## Limites actuelles

- Worker unique (pas de pools multiples)
- Pas de DAG de dépendances
- Pas de priorités
- Aucun Task kind courant ne consomme le scratch externe ; toute future
  éligibilité reste kind-owned et explicitement bornée

Les changements matériels du registre SSD incrémentent la génération Governor
et réveillent ses waiters. Ils ne modifient pas un contrat Task déjà installé,
n'ajoutent pas le swap/scratch à la RAM disponible et ne créent aucune Task.
Une acquisition scratch de production passe par le wrapper Governor, puis sa
release exacte demeure autorisée pendant un drain. Le contrôleur physique ne
rappelle jamais le Governor et aucun mutex Governor n'est tenu pendant l'appel
au contrôleur.

La fermeture Queue bloque l'ingress, attend le worker et tous les appels déjà
enregistrés, puis détruit exactement une fois. Une Task terminale n'est retirée
qu'après retour de son callback terminé ; Queue détruit alors Task/userdata hors
mutex et ne conserve qu'une histoire bornée de 64 snapshots.

## Statut : GATE G — PASS / FROZEN

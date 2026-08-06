# Intégration du scheduler et du gouverneur

## Responsabilités

Le scheduler conserve l'ordre FIFO et exécute les callbacks. Il ne calcule
jamais les budgets : la RAM, le GPU, les CPU, les slots IO et la taille de lot
sont exclusivement arbitrés par le Resource Governor.

Le cycle d'exécution est strict :

```text
Task
  → Resource Estimate
  → Governor
  → Reservation
  → Task Queue
  → Worker
  → Release Reservation
```

Chaque tâche reçoit une estimation immuable à sa création. Son passage de
`PENDING` à `RUNNING` est interdit tant que le gouverneur n'a pas créé une
réservation active. Le callback ne reçoit pas l'objet opaque : il consulte une
copie du contrat contenant le lot, la RAM, la mémoire GPU, les CPU et les slots
accordés.

## Admission

Le worker examine la première tâche FIFO. `WAIT` la laisse en attente et le
worker dort sur la condition de la file. Une libération de ressources suivie de
`lardon3d_task_queue_resources_changed()` le réveille sans polling.
`REDUCE_BATCH` crée un contrat avec le lot réduit, qui est transmis à la tâche.
`REJECT` place la tâche en échec sans appeler son callback et conserve la raison
explicite du gouverneur.

Après succès, échec ou annulation, le worker libère exactement une fois la
réservation puis réveille la file. La destruction annule les tâches, rejoint le
worker et libère toute réservation détenue avant de détruire les tâches.

## Pause

Dans cette première version, une tâche déjà démarrée conserve sa réservation
pendant `PAUSED`. Ses ressources restent donc indisponibles pour les autres
tâches. Ce choix évite de reprendre un callback avec un contrat qui aurait été
attribué entre-temps à un autre travail.

## Limites et extensions

La file possède un seul worker, reste strictement FIFO et ne gère ni priorité
ni dépendance. Une notification explicite est nécessaire lorsqu'un composant
extérieur libère une réservation. Les prochaines étapes pourront ajouter un
DAG, des priorités, des séquences de lots adaptatives et des pools distincts
CPU, IO et GPU sans déplacer les décisions de ressources hors du gouverneur.

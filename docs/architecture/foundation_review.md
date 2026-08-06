# Revue technique des fondations

## Périmètre et conclusion

Cette revue couvre `task`, `task_queue`, le profil matériel, les snapshots de
ressources, le gouverneur, les réservations et leur intégration au scheduler.
Aucune fuite, course, interblocage ou violation reproductible de l'invariant
d'admission n'a été détecté par l'inspection et les tests actuels. Aucun code de
production n'a donc été modifié.

## Invariants actuellement garantis

- Une tâche possède une estimation copiée à sa création et exposée seulement
  par copie.
- Le scheduler reçoit explicitement son gouverneur et refuse un gouverneur nul.
- Le worker obtient une réservation active avant d'appeler
  `lardon3d_task_start`; celui-ci revalide la réservation avant le callback.
- Un callback lancé par la file dispose d'une copie cohérente de son contrat :
  lot, RAM, GPU, CPU et slots IO/GPU.
- `WAIT` conserve la tâche en tête et endort le worker sur une condition
  variable. Le mutex de file empêche une notification concurrente de se perdre
  entre la décision et l'attente.
- `REJECT` termine la tâche sans appeler son callback. `REDUCE_BATCH` transmet
  le contrat réduit.
- La réservation détenue par le worker est libérée après succès, échec ou
  annulation. Une pause en cours conserve volontairement la réservation.
- La destruction de la file annule les tâches, réveille et rejoint le worker,
  puis détruit les tâches dont elle est propriétaire.
- Les compteurs du gouverneur et la création des réservations sont protégés par
  un mutex unique. Une double libération est refusée sans débiter les budgets.
- Les calculs de taille contrôlent multiplication et addition ; les compteurs
  CPU et slots ne peuvent croître au-delà des budgets calculés.
- Le profil représente les capacités stables. Les snapshots sont des valeurs
  datées et indépendantes ; le scheduler ne prend aucune décision de ressources
  lui-même.

## Limites connues

- La file possède un seul worker et applique un FIFO strict. Une tâche en tête
  qui reçoit `WAIT` bloque les tâches suivantes, même si certaines seraient
  admissibles.
- Une libération extérieure au scheduler exige ensuite un appel à
  `lardon3d_task_queue_resources_changed`. Le gouverneur ne publie pas encore
  automatiquement cet événement.
- Les réservations libérées restent comme tombstones jusqu'à la destruction du
  gouverneur. Cela sécurise la double libération mais fait croître la mémoire
  avec le nombre historique de contrats.
- Une erreur de capture du snapshot fait échouer la tâche ; il n'existe pas
  encore de distinction entre erreur transitoire de mesure et rejet durable.
- Le gouverneur et la file doivent être détruits après arrêt de leurs appelants.
  Leur destruction concurrente avec une API active n'est pas prise en charge.
- Les tâches, checkpoints, files et réservations ne sont pas persistés.
- Il n'existe ni DAG, ni priorité, ni pool de workers, ni orchestration de
  séquences adaptatives successives.

## Risques à surveiller

- Formaliser l'ordre de durée de vie : la file doit être détruite avant son
  gouverneur ; une tâche cédée à la file ne doit plus être détruite directement.
- Ne jamais permettre à un composant extérieur de libérer la réservation privée
  du worker. L'appel `get_active` et le démarrage sont sûrs dans le modèle de
  propriété actuel, pas face à une libération concurrente volontaire.
- Éviter qu'un callback détruise ou joigne sa propre tâche, ce qui pourrait
  attendre sa propre fin.
- Conserver les prédicats autour de chaque attente de condition et maintenir le
  même mutex pour décision `WAIT` et mise en sommeil.
- Surveiller les identifiants et compteurs historiques sur les très longues
  sessions, même si leur débordement est irréaliste avec les allocations
  actuelles.
- Ne pas transformer `MemAvailable`, le swap ou la zram en promesse de mémoire
  supplémentaire. Les snapshots peuvent déjà refléter une consommation réelle
  en plus des réservations comptables ; une politique future doit rester
  conservatrice.
- Garder la publication de résultats indépendante du contrat d'exécution : seul
  un résultat validé atomiquement peut devenir visible.

## Cohérence documentaire

Les documents actuels correspondent au code : responsabilités séparées,
réservation préalable, pause conservant les ressources, worker unique et
notification explicite. La vue d'ensemble décrit comme futurs — et non comme
existants — les séquences complètes, la reprise persistante, les snapshots live
et le viewer Vulkan.

## Ordre recommandé des prochains tickets

1. Formaliser les contrats de propriété, les événements de libération et les
   erreurs transitoires de snapshot.
2. Définir un format de résultat atomique avec identifiant, validation et point
   de reprise.
3. Ajouter l'enchaînement borné de lots adaptatifs sous réservations successives.
4. Borner les files et introduire la contre-pression.
5. Persister tâches et checkpoints nécessaires à la reprise après crash.
6. Ajouter ensuite un DAG minimal, puis les priorités.
7. Généraliser vers des pools CPU, IO et GPU en conservant le gouverneur comme
   unique arbitre.
8. Publier des snapshots validés avant d'introduire le viewer séparé.

## Éléments à ne pas réécrire lors du passage à OpenCode

- Les structures opaques `Task`, `TaskQueue`, `ResourceGovernor` et
  `ResourceReservation`.
- La séparation profil matériel / snapshot dynamique / politique / réservation.
- Le calcul centralisé et protégé des budgets et lots.
- L'invariant « réservation active avant callback » et la copie du contrat vers
  la tâche.
- L'annulation coopérative, les checkpoints de pause et la propriété ncurses du
  thread principal.
- Le FIFO à condition variable comme implémentation V1 fiable ; il doit évoluer
  par extension, pas être remplacé avant que les besoins DAG soient spécifiés.
- Les écritures atomiques, rollbacks ciblés et validations déjà utilisés par les
  projets et imports.
- Les tests de concurrence, de double libération, d'annulation et de destruction
  sûre, qui constituent la base de non-régression.

## Validation exécutée

- Suite normale : 10 tests réussis sur 10.
- ASan/UBSan : 10 tests réussis sur 10, aucun diagnostic.
- TSan : 10 tests réussis sur 10, aucune course signalée.
- `git diff --check` : réussi avant la rédaction du présent rapport.

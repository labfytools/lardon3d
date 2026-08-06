# Vue d'ensemble de l'architecture Lardon3D

## Finalité et flux global

Lardon3D est une application Linux de reconstruction 3D pilotée par une TUI
ncursesw. Le terminal reste le centre de contrôle : il gère les projets, lance
les opérations, présente leur progression et permet leur annulation. Le futur
viewer Vulkan sera un processus ou composant graphique séparé, affiché sur le
workspace 8 ; il ne remplacera pas la TUI et ne devra jamais la bloquer.

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

Les projets persistants regroupent leur configuration, les images originales,
le manifeste, les résultats de reconstruction, les exports et les journaux.
Leur création est protégée contre l'écrasement et les écritures structurantes
utilisent des remplacements atomiques.

L'import d'images s'exécute de manière asynchrone et annulable, sans appel
ncurses depuis son worker. Il copie individuellement les fichiers admissibles
et maintient un manifeste cohérent. Le catalogue charge et valide ce manifeste
en mémoire. La vue d'images en dérive des indices triés et filtrés sans modifier
le catalogue, le manifeste ou les images.

Le moteur de tâches fournit les états, la progression, la pause, l'annulation
coopérative et les callbacks. La file actuelle possède un worker unique et
respecte l'ordre FIFO. Chaque tâche porte une estimation immuable de ses coûts
RAM, GPU, CPU et IO ainsi que des bornes de lot.

Le profil matériel décrit les capacités stables détectées sur la machine. Les
snapshots décrivent les ressources disponibles à un instant donné. Le Resource
Governor combine profil, snapshot, marges de sécurité et réservations actives.
Il décide si une demande doit démarrer, attendre, réduire son lot ou être
refusée, puis matérialise toute admission par une réservation opaque.

Le scheduler ne décide jamais des ressources. Il demande une réservation au
gouverneur juste avant l'exécution et transmet au callback une copie du contrat
accordé. L'invariant est strict : aucun callback de tâche n'est lancé sans
réservation active validée. Après succès, échec ou annulation, cette réservation
est libérée exactement une fois. Une tâche déjà en pause conserve son contrat
dans cette première version.

## Résultats et publication live

Les traitements futurs fonctionneront par séquences adaptatives : lire un lot
borné, calculer, écrire un résultat atomique, libérer la mémoire, puis traiter
le lot suivant. La stabilité du système hôte et la réactivité de la TUI ont
priorité sur le débit maximal.

Le viewer live ne devra observer que des snapshots de résultats complètement
validés et publiés atomiquement. Il ne lira jamais un fichier intermédiaire et
ne partagera pas directement les buffers de travail d'un worker. Une
interruption doit laisser le dernier snapshot validé exploitable et permettre
la reprise à une frontière de séquence connue.

## Principes non négociables

- Aucune tâche lourde monolithique ni chargement complet d'un projet en RAM.
- Traitement par séquences adaptatives et libération entre les lots.
- Budgets RAM, GPU, CPU et IO explicitement bornés et réservés.
- Files de travail et buffers intermédiaires bornés.
- La zram est un filet de sécurité, jamais une extension du budget normal.
- La RAM partagée des iGPU est comptabilisée dans le budget système.
- Écritures atomiques, rollback ciblé et absence de résultat partiellement
  publié.
- Reprise après interruption depuis le dernier état validé.
- Viewer live séparé, non bloquant et lecteur de snapshots validés seulement.
- Le système hôte, la TUI et les données utilisateur restent prioritaires sur
  le débit de reconstruction.

## Limites actuelles

La file ne possède encore ni DAG, ni priorités, ni pool de workers CPU/IO/GPU.
Les tâches et leur progression ne sont pas persistées après un arrêt. Les
séquences adaptatives sont préparées par les contrats de lot mais leur
enchaînement complet n'est pas encore orchestré. Le viewer Vulkan et la
publication live restent à implémenter.

## Ordre recommandé des prochains tickets

1. Définir les résultats atomiques, leurs métadonnées de validation et leurs
   points de reprise.
2. Introduire l'exécution d'une tâche en séquences de lots adaptatifs, toujours
   sous réservations successives.
3. Borner explicitement les files et définir la contre-pression entre étapes.
4. Persister les tâches, checkpoints et états nécessaires à la reprise après
   crash.
5. Ajouter un DAG minimal et seulement ensuite les priorités.
6. Introduire des pools CPU, IO et GPU sans déplacer l'arbitrage hors du
   gouverneur.
7. Publier des snapshots live validés et versionnés.
8. Ajouter le viewer Vulkan séparé sur le workspace 8 comme consommateur en
   lecture seule de ces snapshots.

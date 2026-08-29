# Resource Governor Lardon3D

## SIFT v1A

Une extraction SIFT demande jusqu'à douze threads CPU, un slot IO, aucun GPU,
pour une image. Le Governor réduit ce plafond au budget hôte, identique à la
limite OpenCV process-wide configurée avant les workers. L'estimation
structurelle conservatrice est environ 1,06 Gio (décodage,
pyramides, candidats et F32×128), lot 1, pic de record batch zéro. La
réservation couvre ainsi le fan-out interne sans créer un second pool runtime.

## Responsabilité

Le Resource Governor est l'unique propriétaire des budgets (RAM, GPU, CPU,
IO). Il arbitre les ressources disponibles et calcule les lots adaptatifs pour
chaque tâche.

Le profil interactif par défaut conserve un quart de la RAM détectée et un
quart des threads logiques pour le système hôte. Sur 16 Gio/16 threads, cela
donne environ 3,8 Gio et 4 threads de headroom. Une nouvelle admission attend
également lorsque PSI CPU `some avg10` atteint 20 %, ou PSI mémoire 1 %. Ces
signaux n'interrompent jamais le petit job déjà réservé.

La soft floor vaut un quart et la hard floor un huitième de la RAM détectée. La
soft floor place le Governor au minimum en YELLOW ; la hard floor le place
immédiatement en RED. Le premier delta swap entre deux snapshots produit
YELLOW. Un second delta consécutif produit RED. Le premier snapshot ne constitue
qu'une baseline et n'est jamais interprété comme une activité récente.

La récupération interdit `RED → GREEN` : trois observations saines produisent
RED vers YELLOW, puis trois autres YELLOW vers GREEN. Le plafond reste 1 pendant
ces phases. Une fois GREEN, chaque groupe de trois observations saines double
le plafond : 1, 2, 4, 8, puis les paliers supérieurs utiles aux autres kinds.

## Contrat Gate G gelé

**PASS / FROZEN.** Les constantes existantes ci-dessus
restent inchangées. La RAM disponible conserve le modèle conservateur
`min(MemAvailable, RAM physique) - réserve hôte - réservations actives`, borné à
zéro. Le double comptage conservateur possible d'une allocation déjà visible
dans `MemAvailable` est accepté : un faux `WAIT` est préféré à un overcommit.

Les snapshots de production emploient `CLOCK_MONOTONIC` et sont valides jusqu'à
un âge exact de 1000 ms inclus. Un snapshot plus ancien ou daté dans le futur
produit `WAIT`, sans réservation ni mutation de l'état de politique du
Governor. La capture synchrone complète impossible reste une erreur
opérationnelle qui fait échouer la tâche avant callback. Une télémétrie PSI ou
vmstat optionnelle absente reste inconnue et ne crée aucune pression fictive.

Gate G core cible un processus Linux natif non contraint et n'est pas cgroup,
systemd `MemoryMax` ou RLIMIT-aware. Il gouverne un seul GPU : le périphérique
DRM de plus petit numéro retenu par Hardware Profile. La capacité et l'usage
doivent provenir de ce même périphérique. La mémoire UMA est débitée exactement
une fois du budget RAM. Le multi-GPU est différé.

Le Governor ne garantit aucune allocation et ne transforme ni swap, ni zram,
ni stockage externe en RAM. Il ne modifie aucun paramètre scientifique. Aucun
scratch, cache de télémétrie, suivi RSS, redimensionnement de réservation ou
monitoring live n'appartient à Gate G core.

## API principale

### Création et destruction
- `lardon3d_resource_governor_create()` - Créer un gouverneur
- `lardon3d_resource_governor_destroy()` - Détruire un gouverneur

### Configuration
- `lardon3d_resource_governor_set_policy()` - Définir la politique

### Décision
- `lardon3d_resource_governor_decide()` - Décider de l'admission d'une tâche

### Réservation
- `lardon3d_resource_governor_reserve()` - Réserver des ressources
- `lardon3d_resource_governor_reserve_available()` - Réserver les ressources disponibles
- `lardon3d_resource_governor_release()` - Libérer une réservation
- `lardon3d_resource_governor_reservation_is_valid()` - Vérifier la validité

### Métriques
- `lardon3d_resource_governor_availability()` - Obtenir la disponibilité
- `lardon3d_resource_governor_record_batch()` - Enregistrer les métriques d'un lot
- `lardon3d_resource_governor_generation()` - Obtenir la génération actuelle
- `lardon3d_resource_governor_wait_for_change()` - Attendre un changement
- `lardon3d_resource_governor_pressure()` - Lire GREEN, YELLOW ou RED

## Invariants

1. Le scheduler ne décide jamais des ressources
2. Le Resource Governor est l'unique propriétaire des budgets
3. Les réservations sont obligatoires avant toute exécution
4. Les réservations sont libérées exactement une fois
5. Les estimations de ressources sont immuables
6. L'historique des métriques est strictement borné (8 entrées par classe)

## Cycle de vie

```text
1. Capture d'un snapshot de ressources
2. Décision d'admission
3. Réservation opaque
4. Exécution de la tâche
5. Enregistrement des métriques
6. Libération de la réservation
```

## Adaptation dynamique

### Calcul de lots adaptatifs
- Basé sur la consommation mémoire historique
- Mise à jour à chaque exécution
- Conservative (sous-estimation plutôt que sur-estimation)

### Historique borné
- 8 entrées par classe de tâche
- Buffer circulaire
- Mise à jour FIFO

## Réserves

- Sous-estimation temporaire possible avec des estimations statiques
- Pas d'adaptation basée sur le débit (duration_ns non encore utilisé)
- L'import `import.images` est admis avec 128 Kio fixes, un coût borné par item,
  un thread CPU, un slot I/O et des lots de 1 à 32. Il enregistre le nombre
  d'images logiques nouvellement enregistrées dans le ScanSet et la durée
  réelle du lot.
  Cela inclut une copie orpheline identique adoptée, même si aucun octet n'est
  recopié. `peak_memory_bytes == 0` signifie explicitement « mesure inconnue » :
  l'échantillon peut conserver taille/durée mais n'alimente jamais l'adaptation
  mémoire.
- Pas de communication inter-classes de tâches
- `features.extract` réserve un lot de 1, demande jusqu'à douze threads CPU et
  un slot I/O, avec
  64 Mio fixes et 512 Mio par image. Cette estimation conservatrice couvre le
  chemin actuel sans prétendre mesurer les allocations internes d'OpenCV.
  `record_batch` couvre la validation source, le décodage, ORB, la publication
  et la finalisation DB ; `peak_memory_bytes == 0` signifie « mesure inconnue ».
- `visual_index.update` demande jusqu'à douze threads CPU, un slot I/O, 8 Mio
  fixes et 2 Mio par Feature Set, par lots de 1 à 16. Le GPU vaut zéro. Le
  callback compte comme participant et crée au plus `cpu_threads - 1` enfants,
  tous joints avant publication et rupture de séquence. Chaque participant
  possède au plus un reader/FD Feature File ; les tranches de postings privées
  partitionnent le buffer borné du segment. `record_batch` compte uniquement
  les memberships commités et conserve la mémoire inconnue à zéro.
- `candidate_pair.generate` demande jusqu'à douze threads CPU, un slot I/O,
  256 Kio fixes et 64 Kio par Feature Set, par lots de 1 à 64. Le GPU vaut
  zéro. La reconstruction reconnaît uniquement l'ancienne estimation exacte
  (128 Kio fixes, 64 Kio par item, un thread CPU, un slot I/O, aucun GPU, lots
  1 à 64, classe CPU) et la normalise éphémèrement vers l'estimation courante
  complète ; aucun checkpoint d'estimation seule n'est publié et une forme
  voisine n'est jamais réinterprétée comme legacy.
  `record_batch` compte le nombre de paires générées par séquence et la durée
  réelle du lot ; `peak_memory_bytes == 0` signifie « mesure inconnue ».
  Chaque séquence interroge le Visual Index pour jusqu'à 64 memberships. Le
  calcul emploie des fenêtres internes d'au plus deux sources par thread admis
  et 24 sources au total ; le propriétaire de Task persiste ensuite seul et en
  ordre canonique. Cette estimation opérationnelle ne limite pas la taille
  scientifique du dataset.
- `matcher.run` demande jusqu'à huit threads CPU, un slot IO et 10 Mio par
  Candidate Pair admise, correspondant au working set contrôlé inférieur à
  environ 10 Mio par paire au
  maximum SIFT/RootSIFT (8 Mio de descripteurs contigus, KNN `k=2`, sorties et
  fichier bornés), hors scratch interne OpenCV. Ses lots sont bornés à 1, 2, 4
  ou 8 Candidate Pairs. **IMPLEMENTATION IN PROGRESS — P4 :** une fenêtre
  contient au plus deux paires par thread CPU effectivement admis et huit
  paires au total. Le callback Queue est un participant, crée au plus
  `cpu_threads - 1` enfants et les joint avant publication et libération de la
  réservation. Chaque paire conserve ses buffers dans un stage privé jusqu'à
  sa publication ordonnée ou son nettoyage ; OpenCV reste à un thread interne
  pour éviter une sursouscription imbriquée. Le Governor réserve donc au plus
  80 Mio contrôlés pour un lot de huit ; cette borne opérationnelle ne limite
  pas la cardinalité scientifique du dataset.
  Le mode d'exécution est fixé par l'estimation immutable avant admission. Le
  mode parallèle par défaut est CPU-only et ne réserve aucun GPU, même si le
  Governor réduit ensuite son admission à un thread. Un mode explicitement
  sériel ORB, avec profil GPU et backend runtime disponibles, demande exactement
  un thread CPU, un slot GPU et 640 Kio ; sur UMA ces 640 Kio sont aussi débités
  du budget RAM. Sa reconstruction conserve ce mode à partir de l'estimation
  durable, sans sélection tardive d'une ressource non réservée.
  La reprise accepte les formes courantes exactes CPU8/GPU0 et
  CPU1/GPU1/640 Kio ainsi que leurs prédécesseurs exacts CPU12. Elle normalise
  éphémèrement CPU12 vers la forme courante du même mode avant admission. Toute
  forme voisine est rejetée ; cette compatibilité opérationnelle ne persiste ni
  ne déduit une identité backend dans Project DB.
- `track_builder.run` réserve un worker CPU, aucun GPU et aucun fan-out GVR.
  L'estimation est `4 MiB + raw_inlier_edges * (48 + 2*160)` avec facteur 2
  sous 400000 arêtes et facteur 8 au-delà, après vérification d'overflow. Le
  facteur élevé protège la transition mémoire observée à grande échelle ; le
  Governor reste l'unique propriétaire de l'admission et de la pression.

## Limites actuelles

- Worker unique (pas de pools multiples)
- Pas de priorités entre tâches
- Pas de persistance des métriques
- Pas de communication avec d'autres gouverneurs

Les corrections dérivables G-D01 (`UINT64_MAX` est le dernier ID valide et la
création suivante échoue sans réservation ni charge comptable),
G-D02 (saturation des compteurs de streak) et G-D03 (identité DRM identique
entre capacité et usage) sont implémentées. Les sept décisions G-B01 à G-B07
sont gelées ; il ne reste aucune décision humaine Gate G.

## Statut

**GATE G — PASS / FROZEN.**

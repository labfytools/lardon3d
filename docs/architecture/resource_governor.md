# Resource Governor Lardon3D

## SIFT v1A

Une extraction SIFT réserve séparément un slot CPU et un slot IO, aucun GPU,
pour une image. L'estimation structurelle conservatrice est environ 1,06 Gio
(décodage, pyramides, candidats et F32×128), lot 1, pic de record batch zéro.
OpenCV peut employer son parallélisme interne ; aucun état global n'est modifié.

## Responsabilité

Le Resource Governor est l'unique propriétaire des budgets (RAM, GPU, CPU, IO). Il arbitre les ressources disponibles et calcule les lots adaptatifs pour chaque tâche.

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
- `features.extract` réserve un lot de 1, un thread CPU et un slot I/O, avec
  64 Mio fixes et 512 Mio par image. Cette estimation conservatrice couvre le
  chemin actuel sans prétendre mesurer les allocations internes d'OpenCV.
  `record_batch` couvre la validation source, le décodage, ORB, la publication
  et la finalisation DB ; `peak_memory_bytes == 0` signifie « mesure inconnue ».
- `visual_index.update` réserve un thread CPU, un slot I/O, 8 Mio fixes et
  2 Mio par Feature Set, par lots de 1 à 16. Le GPU vaut zéro. `record_batch`
  compte uniquement les memberships commités et conserve la mémoire inconnue à zéro.
- `candidate_pair.generate` réserve un thread CPU, un slot I/O, 128 Kio fixes
  et 256 Kio par Feature Set, par lots de 1 à 64. Le GPU vaut zéro.
  `record_batch` compte le nombre de paires générées par séquence et la durée
  réelle du lot ; `peak_memory_bytes == 0` signifie « mesure inconnue ».
  Chaque séquence interroge le Visual Index pour jusqu'à 64 Feature Sets et
  persiste les paires candidates avec idempotence.

## Limites actuelles

- Worker unique (pas de pools multiples)
- Pas de priorités entre tâches
- Pas de persistance des métriques
- Pas de communication avec d'autres gouverneurs

## Statut : IMPLEMENTED

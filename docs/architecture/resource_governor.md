# Gouverneur de ressources

## Rôle

Le gouverneur protège la réactivité de Lardon3D et la stabilité du système. Les
étapes de photogrammétrie peuvent durer plusieurs heures et manipuler des jeux
de données plus grands que la mémoire disponible. Autoriser chaque tâche à
choisir seule ses ressources conduirait à des pointes de RAM, à la saturation de
la mémoire GPU partagée et à une concurrence incontrôlée sur le stockage.

Le scheduler et le gouverneur ont donc des responsabilités distinctes. Le
gouverneur décide quelles ressources peuvent être promises. Le scheduler décide
quand et sur quel worker exécuter le travail autorisé. Le scheduler ne doit pas
interpréter lui-même la RAM disponible, la charge CPU ou la pression IO.

## Contrat d'exécution

Le chemin d'admission est le suivant :

```text
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

Une décision seule est une observation périssable. Deux threads pourraient
observer le même budget et tous deux démarrer, alors que leur consommation
cumulée dépasse la capacité. Une réservation résout cette course : le calcul du
lot et l'inscription dans les budgets sont effectués atomiquement sous le mutex
du gouverneur. Seule une réservation active constitue une autorisation
d'exécution.

`Lardon3DResourceEstimate` décrit la demande sans être modifié par le
gouverneur : coûts fixes, coûts par élément, bornes du lot, threads et slots
souhaités, et classe de tâche. `Lardon3DResourceReservation` est opaque. Son
instantané public décrit exactement le contrat retenu, y compris le lot réduit.

## Cycle de vie

1. Le producteur construit une estimation pour une unité de pipeline.
2. Un instantané des ressources dynamiques est capturé.
3. Le gouverneur compare la demande aux marges, aux ressources disponibles et
   aux réservations actives.
4. Une demande impossible est refusée. Une pénurie temporaire demande
   d'attendre. Une demande admissible produit une réservation, éventuellement
   avec un lot ou des slots réduits.
5. Le scheduler vérifie que la réservation est toujours active avant de confier
   la tâche à un worker.
6. Le worker respecte strictement le lot, les threads et les slots réservés.
7. À la fin, après annulation ou après échec, la réservation est libérée une
   seule fois. Les budgets redeviennent immédiatement disponibles.
8. Le gouverneur détruit les objets de réservation restants lors de son propre
   arrêt. Les pointeurs de réservation ne doivent plus être utilisés ensuite.

Une seconde libération est refusée sans modifier les compteurs. Les
réservations libérées sont conservées comme tombstones jusqu'à la destruction
du gouverneur afin de détecter cette erreur de cycle de vie sans accès mémoire
invalide.

## Budgets

### RAM

Le budget part de la mémoire actuellement disponible, bornée par la mémoire
physique détectée, puis retire la marge système et toutes les réservations
actives. Les coûts fixes et par élément sont comptabilisés avec contrôle des
débordements. Pour un GPU à mémoire partagée, les besoins RAM et GPU sont
additionnés dans cette même enveloppe.

### GPU

Lorsque la VRAM dédiée est mesurable, sa marge et ses réservations ont un budget
distinct. Si sa disponibilité est inconnue, une tâche qui en dépend attend au
lieu de supposer une capacité. Les slots GPU limitent aussi le nombre
d'opérations concurrentes, indépendamment du nombre d'octets annoncé.

### CPU

Une partie des CPU logiques reste réservée au système et à la TUI. Le gouverneur
borne les threads souhaités par les threads encore disponibles. La charge
moyenne peut différer une nouvelle admission sans interrompre les travaux déjà
réservés.

### IO

Les slots IO limitent les opérations lourdes concurrentes. La pression PSI
Linux peut différer une tâche afin de conserver un terminal réactif et d'éviter
une file d'attente disque excessive.

### Swap et zram

L'instantané mesure le swap disponible, y compris la zram exposée comme swap
par Linux. Ce volume est un signal de sécurité et non une extension normale du
budget RAM : le gouverneur ne doit pas dimensionner un lot en comptant sur le
swap. Une future politique pourra utiliser sa baisse pour suspendre les
admissions ou réduire davantage les lots.

## Lots adaptatifs

Le gouverneur calcule le plus grand lot compris entre les bornes de
l'estimation et compatible avec tous les budgets restants. Une demande de 128
éléments peut ainsi produire un contrat de 37 éléments. Le scheduler répétera
le travail sur plusieurs lots, avec écriture des résultats et libération de la
mémoire entre chaque lot. Cette règle privilégie une progression régulière à
une allocation monolithique.

Les coûts fixes ne sont payés qu'une fois par lot ; les coûts par élément
déterminent sa capacité maximale. Les slots et threads recommandés font partie
du même contrat et ne doivent pas être augmentés par le worker.

### Adaptation par les métriques

Le gouverneur peut additionally utiliser les métriques de lots déjà exécutés
pour réduire conservativement la taille maximale des lots futurs. L'adaptation
complète les estimations statiques ; elle ne les remplace pas.

Le principe est le suivant : si le coût mémoire réel observé dépasse l'estimation
déclarée, le gouverneur réduit la taille du lot futur afin que sa consommation
reste dans le budget. Le coût par élément le plus défavorable observé est retenu
afin de ne jamais sous-estimer la charge.

Propriétés de l'adaptation :

- Le calcul du coût par élément utilise une division entière sans overflow :
  `quotient + (reste != 0)`.
- L'adaptation ne peut que réduire la taille maximale du lot ; elle ne
  l'augmente jamais.
- `minimum_batch_size` reste garanti quelle que soit l'adaptation.
- Les métriques restent isolées par classe de tâche.
- L'historique est strictement borné (voir ci-dessous).

## Métriques de lots

Par classe de tâche, le gouverneur conserve un historique circulaire borné à
8 entrées. Chaque entrée enregistre :

- `batch_size` : taille du lot exécuté ;
- `duration_ns` : durée d'exécution (collectée pour une adaptation future basée
  sur le débit, mais n'influence pas encore les décisions) ;
- `peak_memory_bytes` : pic de mémoire mesuré pendant l'exécution du lot.

L'enregistrement est thread-safe : il est protégé par le mutex du gouverneur.
La génération est incrémentée et les threads en attente sont réveillés
uniquement lorsqu'une métrique est réellement enregistrée. Un `batch_size` égal
à 0 est traité comme un no-op réussi : la fonction retourne `true` sans modifier
les métriques ni la génération.

L'historique étant borné, les anciennes entrées sont écrasées par les plus
récentes. Cette contrainte garantit un temps et une mémoire d'exécution bornés.

## API record_batch

```text
bool lardon3d_resource_governor_record_batch(
    Lardon3DResourceGovernor *governor,
    Lardon3DResourceTaskClass task_class,
    size_t batch_size,
    uint64_t duration_ns,
    size_t peak_memory_bytes
);
```

Contrat :

- **Thread-safe** : protégé par `governor->mutex`.
- `governor` NULL → retourne `false`.
- `task_class` invalide (supérieur à `LARDON3D_RESOURCE_TASK_MIXED`) → retourne
  `false`, aucun effet secondaire.
- `batch_size` == 0 → retourne `true`, no-op : aucune métrique enregistrée,
  aucune modification de la génération.
- Enregistrement valide → métrique ajoutée à l'historique circulaire de la
  classe, génération incrémentée et threads en attente réveillés par broadcast.

## Concurrence des métriques

Les invariants de concurrence suivants ont été validés par TSan :

- Les métriques sont protégées par `governor->mutex`.
- La lecture adaptative (`adaptive_batch_limit`) s'effectue sous ce même verrou.
- Aucun mutex supplémentaire n'est introduit.
- Aucune allocation dynamique n'est effectuée pour l'historique (tableau statique
  de taille fixe par classe).
- Aucun broadcast n'est émis pour un no-op ou une entrée invalide.
- L'historique étant borné (8 entrées par classe), le temps et la mémoire
  d'exécution restent bornés.

## État d'intégration

**Implémenté :**

- API `record_batch` et son contrat thread-safe.
- Stockage des métriques dans un historique circulaire borné.
- Adaptation mémoire conservative de `maximum_batch_size`.
- Réveil via génération et broadcast.
- Tests unitaires et validation TSan.

**Pas encore câblé :**

- Appel systématique de `record_batch` depuis les tâches et pipelines réels.
- Exploitation de `duration_ns` pour une adaptation basée sur le débit.

## Utilisation future

Le même protocole s'applique aux imports, miniatures et extractions EXIF, puis
aux features, au matching, au SfM, aux depth maps, au mesh et aux textures. Les
estimations pourront différer par classe sans déplacer les décisions dans le
scheduler. Le viewer Vulkan utilisera également une réservation GPU afin de ne
pas concurrencer silencieusement une reconstruction sur une machine à mémoire
partagée.

L'API `record_batch` sera câblée dans les tâches réelles du pipeline afin
d'alimenter les métriques d'adaptation. L'exploitation de `duration_ns` pour
une adaptation basée sur le débit est prévue pour une itération ultérieure.

Cette séparation permettra ultérieurement plusieurs workers : chacun recevra
un contrat déjà arbitré, tandis que le gouverneur restera l'unique propriétaire
des budgets globaux.

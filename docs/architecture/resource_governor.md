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

## Utilisation future

Le même protocole s'applique aux imports, miniatures et extractions EXIF, puis
aux features, au matching, au SfM, aux depth maps, au mesh et aux textures. Les
estimations pourront différer par classe sans déplacer les décisions dans le
scheduler. Le viewer Vulkan utilisera également une réservation GPU afin de ne
pas concurrencer silencieusement une reconstruction sur une machine à mémoire
partagée.

Cette séparation permettra ultérieurement plusieurs workers : chacun recevra
un contrat déjà arbitré, tandis que le gouverneur restera l'unique propriétaire
des budgets globaux.

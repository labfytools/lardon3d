# Pipeline Feature + Matcher sensible aux ressources

## Contrat portable

Une unité lourde ne démarre qu'avec une réservation active. Elle termine son
petit travail courant sans être tuée sur une mesure instantanée, publie le
résultat atomiquement, checkpoint, libère ses buffers, puis repasse par le
Governor avant la séquence suivante. Les files restent bornées et le swap n'est
jamais ajouté au budget de travail.

Le mode normal est interactif : il réserve de la RAM et des threads logiques au
desktop. Les signaux d'admission combinent `MemAvailable`, charge CPU, PSI CPU,
PSI mémoire, PSI I/O et deltas `pswpin`/`pswpout`. Un seuil dépassé empêche une
nouvelle admission ; il ne rompt pas une réservation saine déjà active.

Le Governor maintient trois zones. GREEN emploie le lot adapté normal. La soft
floor RAM, un PSI au seuil ou un premier intervalle avec swap actif produit
YELLOW et interdit toute croissance. Deux observations de pression
consécutives, ou `MemAvailable` sous la hard floor, produisent RED et suspendent
toute admission. Le premier snapshot swap établit seulement la baseline.

La récupération possède deux phases distinctes : trois observations saines
font `RED → YELLOW`, puis trois nouvelles observations saines font
`YELLOW → GREEN`. Après RED, le plafond de lot reste 1. En GREEN, trois
observations saines sont nécessaires à chaque palier `1 → 2 → 4 → 8`. Une
nouvelle pression réinitialise cette progression. Cette mémoire est
process-local, bornée et protégée par le mutex du Governor.

## Feature Extraction

ORB est déjà une tâche durable par image : source validée, extraction,
publication Feature Store, métadonnées DB, checkpoint terminal et libération du
buffer. Le batch vaut donc une image et la granularité de reprise est une image.
Le worker unique et la file bornée fournissent la backpressure actuelle.

OpenCV est configuré une seule fois avant le démarrage des workers. La tâche
réserve le nombre réel de threads OpenCV au lieu d'annoncer artificiellement un
thread pendant qu'une primitive interne en utilise davantage.

## Matcher

`matcher.run` v1 est une tâche durable. Son unité atomique est une Candidate
Pair et son lot vaut 1, 2, 4 ou 8 paires. La tâche page la DB par
`candidate_pair_id`, sans supposer des IDs continus, et ne conserve jamais la
liste entière. Chaque paire publie immédiatement son Match Result avant que le
curseur ne soit avancé en mémoire.

Project DB v10 porte le Match Result publié. La migration transactionnelle
v10→v11 ajoute uniquement `matcher_tasks`, qui porte la configuration et ce
curseur durable.

Après chaque lot, la tâche persiste le curseur, checkpoint, puis appelle
`lardon3d_task_sequence_break()`. Pause et annulation sont vérifiées avant
chaque paire et entre les lots. Un crash après publication mais avant le
checkpoint revoit la paire : le Matcher réutilise alors le Match Result et ne
recalcule pas les descripteurs.

## GPU et files

La Radeon 780M est UMA : toute mémoire GPU compte aussi comme pression RAM. Un
backend GPU emploie un unique job actif, des dispatchs courts, puis publie avant
de continuer. Vulkan 1.4.354 énumère la 780M RADV et une file compute dédiée. Le
backend ORB top-2 de production possède un contexte lazy réutilisable, 640 Kio
de buffers bornés et un fallback CPU exact. Le CPU reste le fallback portable
si Vulkan est absent, incompatible ou désactivé pour la session.

La feasibility SIFT/RootSIFT a borné son prototype Vulkan à 8,125 Mio de
payload lazy, mais n'a pas franchi la Gate de production. Le Governor ne réserve
donc aucun slot ni budget GPU pour SIFT/RootSIFT ; leur estimation CPU publiée
reste inchangée.

## Profil interactif 8845HS mesuré

- budget CPU Lardon3D : 12 threads logiques, 4 réservés au desktop ;
- réserve `MemAvailable` : un quart de la RAM, environ 3,8 Gio ;
- hard floor `MemAvailable` : un huitième, environ 1,9 Gio ;
- Feature workers : 1 ; batch : 1 image ;
- Matcher workers : 1 ; lots adaptatifs 1, 2, 4 ou 8 Candidate Pairs ;
- profondeur de la Task Queue : 64 tâches légères, un seul callback actif ;
- PSI CPU avg10 : nouvelle admission suspendue à 20 % ;
- PSI mémoire avg10 : nouvelle admission suspendue à 1 % ;
- PSI I/O avg10 : seuil existant 80 %.

Le benchmark Matcher 8192 mesure environ 70 ms ORB et 135 ms SIFT à 12 threads,
contre 68 ms et 127 ms à 16 threads : le profil interactif abandonne environ
3–7 % de latence isolée pour réserver quatre threads logiques au desktop.

## Limites

Le profil maximal explicite et les pools multi-workers restent hors périmètre.
SIFT/RootSIFT et Feature Extraction Vulkan restent hors de ce contrat.

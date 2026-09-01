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

Le Governor maintient trois zones. GREEN emploie le lot adapté normal. La zone
de prudence RAM entre 3 et 4 Gio, un PSI au seuil ou un premier intervalle avec
swap actif produit YELLOW et interdit toute croissance. Deux observations de
pression consécutives, ou `MemAvailable` sous la réserve dure de 3 Gio,
produisent RED et suspendent toute admission. Le premier snapshot swap établit
seulement la baseline.

La récupération possède deux phases distinctes : trois observations saines
font `RED → YELLOW`, puis trois nouvelles observations saines font
`YELLOW → GREEN`. Après RED, le plafond de lot reste 1. En GREEN, trois
observations saines sont nécessaires à chaque palier `1 → 2 → 4 → 8`. Une
nouvelle pression réinitialise cette progression. Cette mémoire est
process-local, bornée et protégée par le mutex du Governor.

Gate G gèle le rafraîchissement initial : lorsqu'il existe du travail PENDING
en `WAIT` de ressources, le worker unique de la Task Queue dort au plus 500 ms
avant de rescanner la file et de recapturer les ressources. Un signal explicite
le réveille plus tôt. Cette cadence ne remplace pas les 50 ms existantes d'une
tâche déjà active qui attend sa réadmission à une frontière de séquence.

**Gate G — PASS / FROZEN.** Cette réévaluation bornée, la
fraîcheur des snapshots et l'identité GPU sélectionnée sont raccordées aux
chemins de production existants et leur validation finale est terminée.

Les snapshots emploient `CLOCK_MONOTONIC` et leur âge maximal est 1000 ms. Une
capture complète impossible est une erreur opérationnelle, tandis qu'une PSI
ou télémétrie swap optionnelle absente reste inconnue. Compute Governor v2
observe maintenant le RSS/HWM courant dans un buffer borné, uniquement comme
diagnostic du processus : il ne le confond ni avec la réservation Task ni avec
un coût attribuable. Le modèle cible un hôte Linux natif non contraint ; cgroups,
limites systemd/RLIMIT, multi-GPU, historique/monitoring RSS long terme,
redimensionnement d'admission depuis le RSS et consommation Task du scratch
restent différés. Le contrôleur SSD optionnel gère le cycle de vie physique ;
son état est enregistré auprès du Governor, qui est l'unique orchestrateur des
leases scratch de production. Aucun des quatorze kinds actuels ne les consomme,
et ni le registre, ni les leases, ni le swap ne créent un budget RAM.

## Feature Extraction

ORB est déjà une tâche durable par image : source validée, extraction,
publication Feature Store, métadonnées DB, checkpoint terminal et libération du
buffer. Le batch vaut donc une image et la granularité de reprise est une image.
Le worker unique et la file bornée fournissent la backpressure actuelle.

Le démarrage configure une baseline OpenCV issue du compute-pool réellement
disponible avant la création de Queue. L'unique callback lourd applique ensuite
temporairement le compte CPU immuable admis pour sa séquence, dans
`1..compute-pool`, et restaure la baseline sur toute
sortie, y compris après une mutation suivie d'un échec de vérification. La tâche
réserve donc le nombre réellement appliqué au lieu d'annoncer artificiellement
un thread pendant qu'une primitive interne en utilise davantage. Une mutation
process-wide concurrente par plusieurs workers n'est pas supportée ; Queue
conserve un seul callback actif.

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

## Geometric Verification

Project DB v12 stocke un résultat borné à 1024 octets de masque et neuf
binary64. `geometric_verifier.run` v1 traite chaque Match Result comme unité
scientifique atomique, publie par une transaction courte puis checkpoint son
curseur par lots 1..16 avant
`task_sequence_break()`. Sa ligne durable appartient à Project DB v13. Le job
peut employer jusqu'à huit participants utiles et seize participants sûrs, par
lots au plus seize, avec 8 Mio par parent et sans slot GPU. Le propriétaire
publie le préfixe canonique dans l'ordre. Admission, pression, lots et
slow-start restent exclusivement décidés par Runtime et Governor ; l'USAC
scientifique interne conserve `isParallel=false`.

## GPU et files

La Radeon 780M est UMA : toute mémoire GPU compte aussi comme pression RAM.
Vulkan 1.4.354 énumère la 780M RADV et une file compute dédiée. Le backend ORB
top-2 de production possède un contexte lazy réutilisable et jusqu'à deux jobs
privés en vol sur cette file, sans helper hôte. Chaque slot mappé vaut 640 Kio.
Le backend part de zéro et retient exactement un slot après une initialisation
ou séquence AUTO normale depth 1. Il n'alloue le second que sous un contrat
privé de sûreté/benchmark depth 2 déjà admis, puis le libère avant de franchir
la prochaine admission depth 1. Les
publications restent strictement ordonnées et le fallback CPU reste exact. Le
CPU reste le fallback portable si Vulkan est absent, incompatible ou désactivé
pour la session.

La feasibility SIFT/RootSIFT a borné son prototype Vulkan à 8,125 Mio de
payload lazy, mais n'a pas franchi la Gate de production. Le Governor ne réserve
donc aucun slot ni budget GPU pour SIFT/RootSIFT ; leur estimation CPU publiée
reste inchangée.

## Profil interactif 8845HS mesuré

- budget CPU Lardon3D observé : 12 threads logiques, 4 réservés au desktop ;
- réserve dure `MemAvailable` : 3 Gio ;
- zone de prudence `MemAvailable` : de 3 à 4 Gio ;
- Feature workers : 1 ; batch : 1 image ;
- Matcher workers : 1 ; lots adaptatifs 1, 2, 4 ou 8 Candidate Pairs ;
- Geometric Verifier : un callback propriétaire, jusqu'à 8 participants utiles,
  lots adaptatifs jusqu'à 16 Match Results ;
- profondeur de la Task Queue : 64 tâches légères, un seul callback actif ;
- PSI CPU avg10 : nouvelle admission suspendue à 20 % ;
- PSI mémoire avg10 : nouvelle admission suspendue à 1 % ;
- PSI I/O avg10 : seuil existant 80 %.

Le benchmark Matcher 8192 mesure environ 70 ms ORB et 135 ms SIFT à 12 threads,
contre 68 ms et 127 ms à 16 threads : le profil interactif abandonne environ
3–7 % de latence isolée pour réserver quatre threads logiques au desktop.

Le run soutenu Geometric Verifier traverse environ 2001 parents réutilisés en
5,870 s via Task, DB, checkpoints et Governor. Le processus de test culmine à
25 964 Kio RSS ; `MemAvailable` reste au-dessus de 10,69 Gio et les compteurs
swap restent nuls. PSI avg10 final vaut 0,34 % CPU et 0 % mémoire/I/O. Cette
mesure valide le chemin resource-aware et la reprise ; elle ne prétend pas être
une distribution de latence estimator-only.

## Limites

Les pools multi-workers restent hors périmètre. La capacité CPU portable est
désormais bornée par le compute-pool de l'hôte et la limite intrinsèque du kind,
jamais par un plafond global 12.
SIFT/RootSIFT et Feature Extraction Vulkan restent hors de ce contrat.
Swap, zram et disque externe ne sont jamais ajoutés au budget RAM. Aucun chemin
scratch/spill Task n'appartient à Gate G core. La commande SSD optionnelle est
une intégration opérationnelle additive, pas une admission scientifique.

La validation B3 du modèle Sparse SfM v16 a utilisé des processus frais, un
fixture synthétique de 100 000 landmarks et 500 000 observations, cinq passes
de paging sur 50 000 landmarks, et n'a observé ni OOM, ni swap storm, ni dérive
RSS applicative. Cette preuve concerne la persistance bornée, pas le solveur.

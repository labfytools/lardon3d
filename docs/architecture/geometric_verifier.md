# Geometric Verifier v1

## Scope

Ce document décrit l'exécution scientifique qui transforme un Match Result `MATCHED` en résultat
Fundamental `GEOMETRIC_REJECTED` ou `GEOMETRIC_VERIFIED`. Le contrat persistant reste défini par
[`geometric_verification.md`](geometric_verification.md). Tracks, pose, Essential, compétition
Homography, triangulation et SfM sont hors périmètre.

## Inputs

Le parent DB fournit les deux Feature Set IDs, le compte, le chemin, la taille et le SHA-256 du
Match File. Le reader Feature Store ouvre séparément chaque Feature Set validé et expose les
keypoints par plages d'au plus 256. Le verifier n'a besoin d'aucun descriptor : charger les blocs
ORB ou SIFT/RootSIFT serait inutile et est interdit dans le chemin normal.

Les keypoints persistants portent des coordonnées `binary32`. `x/y` sont exprimés en pixels de
l'image exactement décodée par OpenCV lors de l'extraction, avec origine en haut à gauche et
positions subpixel possibles. Les dimensions décodées sont disponibles dans les métadonnées du
Feature File.

## Fundamental matrix contract

Le seul modèle v1 est une matrice Fundamental 3×3. Une sortie acceptée doit être unique, finie,
de norme non nulle et canonique avant publication. V1 ne projette pas la matrice vers le rang 2.

## Input ordering

L'entrée `i` de l'estimator correspond exactement à l'entrée `i` du Match File :
`feature_index_a` sélectionne le Feature Set A et `feature_index_b` le Feature Set B. Le Match
File impose déjà des indices A strictement croissants ; le verifier ne trie et ne filtre pas les
correspondances. Toute corruption d'index est une erreur d'exécution, jamais un rejet scientifique.

## Coordinate representation

Le stockage source reste `binary32`. Sur 1024 points, bruit 0,75 px et 50 % d'outliers, Point2f et
Point2d ont produit le même masque et la même qualité, en 3,58 et 3,55 ms. La production convertit
vers Point2d pour rendre le calcul et la sortie binary64 explicites, pour 256 Kio au maximum.
Aucune mise à l'échelle par résolution ni conversion de repère n'est appliquée implicitement.

## Algorithm candidates

OpenCV 5 installé expose `FM_RANSAC`, `USAC_DEFAULT`, `USAC_ACCURATE`, `USAC_PROSAC` et
`USAC_MAGSAC`. La shortlist Gate A est FM_RANSAC comme baseline, puis USAC_DEFAULT,
USAC_MAGSAC et USAC_ACCURATE. PROSAC est `NOT_APPLICABLE` en v1 : la distance descriptor est
persistée mais l'ordre canonique suit l'index de query, pas un classement de qualité benchmarké.

## Benchmark methodology

Un corpus synthétique déterministe avec Fundamental ground truth couvrira bruit, outliers,
résolutions, tailles, géométries saines, faibles et adversariales. Les méthodes seront comparées
par précision/recall du masque, erreurs épipolaires, échecs, repeatability, temps et ressources.
Le benchmark lourd restera hors build et suite par défaut. Aucune fixture photo réelle ne sera
revendiquée sans fixture non sensible présente dans le dépôt.

La campagne Gate A du 9 août 2026 utilise OpenCV 5.0.0, Clang 22.1.8, une seed fixe et 32
répétitions. Elle couvre 7 à 8192 points, 0 à 100 % d'outliers, bruit 0 à 1,5 px, 1280×720 à
4000×3000, baseline faible/large, concentration, quasi-colinéarité, planéité, rotation dominante
et duplications. Aucune fixture photo réelle représentative n'existe dans le dépôt.

| Algorithme | P/R 1024, 30 % | P/R 8192, 70 % | Médiane/p95/pire 8192 | Seed locale | Stable 32× |
|---|---:|---:|---:|---|---|
| FM_RANSAC | 0,998/0,720 | 0,993/0,413 | 316,4/318,9/319,7 ms | non | oui observé |
| USAC_DEFAULT | 0,996/0,960 | 0,997/0,959 | 43,0/43,2/44,8 ms | preset non | oui |
| USAC_MAGSAC | 0,993/0,965 | 0,994/0,962 | 11,4/12,0/12,1 ms | preset non | oui |
| USAC_ACCURATE | 0,996/0,960 | 0,996/0,961 | 30,5/32,0/32,1 ms | preset non | oui |
| MAGSAC params v1 | 0,997/0,957 | 0,996/0,894 | 10,8/11,0/11,4 ms | oui | oui |

FM_RANSAC est rejeté pour son recall et son pire temps. DEFAULT et ACCURATE n'améliorent pas assez
la qualité pour leur coût. La production emploie des UsacParams explicites : la seed par appel
prime sur la variation du cas extrême liée à la seed fixe. À bruit 0,75 px/50 % d'outliers, les
seuils 0,5/1,0/1,5/2,0/3,0 donnent des recalls 0,535/0,811/0,961/0,990/1,000 et des precisions
0,996/0,988/0,990/0,986/0,985. Le compromis retenu est 1,5 px.

## Determinism

USAC expose `cv::UsacParams::randomGeneratorState`, un entier par appel, ainsi que les paramètres
de sampling, score, optimisation locale et polishing. Cette API est préférable à une mutation de
`cv::theRNG()` process-global. FM_RANSAC restera une baseline scientifique tant que son contrôle
RNG et sa repeatability n'ont pas été mesurés.

Les cinq candidats ont donné un hash modèle+masque identique sur 32 appels et dans trois processus
distincts. La garantie v1 reste intra-environnement : mêmes octets, ordre, configuration, seed,
OpenCV 5.0.0 et architecture. Aucun bit-exact cross-version ou cross-architecture n'est promis.

## Random seed policy

La policy v1 calcule SHA-256 sur `L3DGVSE1`, le SHA-256 du Match File puis le fingerprint. Les
quatre premiers octets sont décodés little-endian et les 31 bits faibles alimentent
`randomGeneratorState`. La policy est version 1.

## Parameter fingerprint

Le fingerprint v1 est SHA-256 des 84 octets suivants. Les entiers sont little-endian ; les doubles
sont leurs bits IEEE-754 binary64 écrits comme `uint64_t` little-endian. NaN/Inf sont refusés et
le seul champ autorisant zéro signé, `min_inlier_ratio`, normalise `-0.0` en `+0.0`. Aucun octet ne
provient d'un dump de structure.

| Offset | Taille | Champ |
|---:|---:|---|
| 0 | 8 | domaine ASCII `L3DGVFP1` |
| 8 | 4 | version encodage = 1 |
| 12 | 4 | kind FUNDAMENTAL = 1 |
| 16 | 4 | verifier version = 1 |
| 20 | 4 | algorithme USAC_MAGSAC explicite = 1 |
| 24 | 8 | threshold binary64 |
| 32 | 8 | confidence binary64 |
| 40 | 4 | max iterations |
| 44 | 4 | minimum inlier count |
| 48 | 8 | minimum inlier ratio binary64 |
| 56 | 4 | seed policy version |
| 60 | 4 | canonicalisation version |
| 64 | 1 | représentation Point2d = 2 |
| 65 | 1 | sampler uniforme = 0 |
| 66 | 1 | score MAGSAC = 2 |
| 67 | 1 | isParallel = 0 |
| 68 | 1 | LO inner = 1 |
| 69 | 4 | LO iterations = 5 |
| 73 | 4 | LO sample size = 14 |
| 77 | 1 | neighbor grid = 1 |
| 78 | 1 | COV polisher = 3 |
| 79 | 4 | polisher iterations = 3 |
| 83 | 1 | réservé nul |

Le vector golden de la configuration production commence par les 84 octets hexadécimaux
`4c33444756465031...0300000000` et donne le SHA-256
`ddb44bb070c62be66c405946e89cbb49c084f8f30a21d6f408dc239225b7bbd0`. Pour un Match File SHA
composé de 31 octets nuls puis `01`, cette configuration donne la seed décimale `1910542150`.
Les politiques de
ressources, hardware, PSI, lot, worker et réservation CPU ne sont ni des champs ni des entrées.

## Acceptance policy

Un modèle candidat qui échoue à cette policy publie REJECTED avec son masque et son compte
d'inliers, sans matrice. La production exige `inlier_count >= 16` et
`inlier_count / match_count >= 0,20`. Les cas 100 % faux produisent 10/64, 14/256, 26/1024 et
28/4096 inliers, ratio maximal 0,15625. Les scènes saines produisent 45/64, 129/256 et 297/1024 ;
la faible baseline produit 126/256.

## Fundamental matrix canonicalization

La production adopte cette canonicalisation version 1. Les neuf valeurs doivent être finies. La
norme de Frobenius est calculée avec une accumulation `hypot` résistante au débordement ; zéro est
refusé. Le premier coefficient de valeur absolue strictement maximale gagne, donc un tie conserve
le plus petit index ligne-major. Après division, le signe rend ce pivot positif et les zéros signés
sont normalisés à `+0.0`. Sur 8192/70 %, les singular values sont
3,392e-2, 1,374e-4 et 5,915e-24. OpenCV fournit déjà rank-2 à précision numérique. V1 ne calcule
aucune SVD en production, n'impose aucun seuil de rang et n'effectue aucune post-projection rank-2.
Les validations production portent uniquement sur la forme 3×3 unique, la finitude et la norme.

## Inlier mask generation

Le masque OpenCV est validé en type, taille et valeurs, puis converti sans réordonnancement vers
le bitset LSB-first du modèle. Les frontières 7/8/9, 63/64/65 et 8191/8192 sont testées.

Le core conserve strictement l'ordre d'entrée du Match File. Les tests utilisent des indices B
permutés et des masques non contigus ; le bit `i` publié reste l'élément `i` du fichier, jamais
l'index de feature. Les tailles 1, 2, 7, 8, 9, 63, 64, 65, 8191 et 8192, le padding nul et le
popcount sont couverts avec le Model v1 inchangé.

## Scientific rejection

Un nombre de matches inférieur au minimum réel, l'absence de modèle sur entrée valide ou l'échec
de l'acceptance policy produit un résultat scientifique REJECTED cohérent.

Le minimum USAC observé est sept. Moins de sept matches produit un masque zéro sans appel OpenCV ;
sept à quinze peuvent produire une hypothèse mais ne franchissent pas le support production.

## Execution failure

Match/Feature asset absent ou corrompu, index hors bornes, exception OpenCV, OOM, masque malformé,
matrice non finie ou invariant interne invalide échoue dans le Task Runtime. Aucun résultat
scientifique n'est publié dans ces cas.

Le core traduit parent absent/NO_MATCH et Feature Set absent en erreur d'exécution `NOT_FOUND` ;
asset absent, tronqué, hash divergent, ownership ou index incohérent en `CORRUPT` ; exception ou
sortie estimator malformée/non finie en `ESTIMATOR_ERROR` ; `bad_alloc` en `OUT_OF_MEMORY` ; et
échec Model en `DATABASE_ERROR`. Les seams test-only couvrent erreur estimator, mask/matrice
malformés, NaN, OOM et publication. Aucun de ces chemins ne crée de résultat scientifique.

## Resource bounds

Une unité atomique est un Match Result, au maximum 8192 correspondances. Le Match File est borné
à 98 336 octets et le bitset à 1024 octets. Aucun cache global ni préchargement de projet complet
n'est utilisé.

À 8192 matches, les allocations directement contrôlées maximales sont 98 304 octets d'entries,
393 216 octets de keypoints A/B, 262 144 octets de Point2d A/B, 1024 octets de bitset, environ
8192 octets de mask OpenCV et 72 octets de modèle, soit environ 745 Kio hors petits objets et
scratch OpenCV. Aucun descriptor ni matrice A×B n'est lu. Massif mesure 2,445 Mio de heap au pic
du test E2E complet, incluant SQLite, OpenCV, fixtures Feature Store et toutes les séquences de test.
Une réservation conservatrice de 4 Mio par job couvre ce profil mesuré.

## CPU policy

Le parallélisme OpenCV reste configuré process-wide. Le benchmark mesurera les threads réellement
consommés ; le verifier ne change pas `cv::setNumThreads()` par paire.

La campagne a consommé environ 99 % d'un CPU logique : réservation v1 d'un thread et un worker.

## GPU policy

Aucun backend Vulkan n'est implémenté avant profil du chemin CPU final. La décision attendue est
`NOT_JUSTIFIED` si les unités restent sub-millisecondes ou de quelques millisecondes.

Verdict Gate A : `NOT_JUSTIFIED`. Les cas usuels prennent 0,3 à 5,6 ms et le pire MAGSAC local
mesuré reste à 11,4 ms. Aucun backend Vulkan de vérification n'est implémenté.

## Task Runtime

L'audit Gate C conclut que le checkpoint générique v1 est insuffisant : il conserve l'état,
la progression, le compteur de séquences et les temps, mais aucun payload propre au kind. Le
reconstructeur doit retrouver les paramètres scientifiques immuables et le curseur sans les
inventer depuis un fingerprint irréversible.

`geometric_verifier_tasks` est donc la seule raison de Project DB v13. Elle doit conserver
`task_id`, `after_match_result_id`, les sept paramètres de configuration v1 et le fingerprint
calculé à la création pour validation à la reconstruction. Aucun `cv::Mat`, buffer, état RNG,
paramètre Governor ou donnée hardware n'y appartient. La tâche calcule hors transaction et publie
chaque résultat par transaction courte avant avancement du curseur.

Le Task Kind production est `geometric_verifier.run` version 1. Il pagine les Match Results par ID
strictement croissant avec une page de `batch + 1`, et traite des lots Governor 1/2/4/8. Les
parents autres que `MATCHED` avec `match_count > 0` sont seulement traversés par le curseur. Une
unité éligible appelle le core, qui reuse l'identité exacte avant toute lecture d'asset.

WHY GENERIC TASK PERSISTENCE IS INSUFFICIENT: aucun champ de payload métier dans le snapshot v1.

REQUIRED DURABLE FIELDS: configuration scientifique v1, fingerprint et dernier Match Result
publié puis checkpointé.

WHY EXISTING DB CANNOT STORE THEM: `tasks` et `checkpoints` ne portent que le résumé générique ;
aucune table v12 ne possède une ligne 1:1 adaptée à ce Task Kind.

## Checkpoint/recovery

La pagination suit `match_result_id` croissant sans supposer des IDs contigus. Le résultat est
publié avant que `after_match_result_id` avance en mémoire ; le curseur n'est persisté qu'après le
lot. Après chaque lot non terminal, `task_sequence_break()` rend la réservation au Governor.

Le test de crash publie puis interrompt avant checkpoint du curseur, ferme runtime et DB, recharge
le checkpoint antérieur et reconstruit le Task Kind. Le parent est revu, son résultat exact est
réutilisé, puis le curseur progresse.

## Cancellation

Pause et annulation sont coopératives avant chaque parent et entre lots. Une petite estimation
OpenCV engagée finit et publie avant l'arrêt ; aucun résultat scientifique CANCELLED n'est créé.
Les résultats déjà publiés restent durables.

## Backend policy

Un backend n'est transparent pour l'identité que si ses sorties scientifiques sont équivalentes
selon le contrat. V1 possède une seule implémentation CPU de production.

## Core publication and reuse

Le core charge les métadonnées DB, relâche les mutex internes après chaque API, lit les assets et
calcule sans transaction longue, puis appelle une publication Model v1 courte. Une identité exacte
VERIFIED ou REJECTED est retournée avant toute lecture Feature/Match et sans appel estimator. Une
contrainte concurrente déclenche un unique `find` de l'identité, jamais un overwrite ou une
récursion. Changer un paramètre scientifique produit un autre fingerprint et un autre résultat.

Les tests E2E utilisent le vrai Project DB v13, deux Feature Files à 8192 points, des Match Files
hashés, le vrai MAGSAC et le Model v1. VERIFIED est rechargé après close/reopen avec modèle et
masque bit-identiques ; REJECTED conserve son support et est également réutilisé.

## Production algorithm

UsacParams explicites, sampler uniforme, score MAGSAC, non parallèle et seed locale par appel.
Les champs LO et polishing effectifs sont encodés explicitement ; aucun preset enum caché.

## Production parameters

FUNDAMENTAL version 1 ; seuil 1,5 px ; confiance 0,999 ; 5000 itérations ; 16 inliers ; ratio 0,20 ;
seed policy 1 ; canonicalisation 1 ; Point2d. Tous les champs scientifiques appartiennent au
fingerprint version 1.

## Validation

Gate A couvre corpus, comparaison, seed et repeatability. Gate B couvre fingerprint/seed golden,
canonicalisation, mapping bit à bit, frontières d'acceptation, E2E DB, reuse, corruption,
publication, 8192 matches et ASan/UBSan. Gate C couvre Task, publication avant curseur et reprise.

Gate D a exécuté 1000 parents configurés dans la vraie Task, puis les reprises et variantes de
configuration du test : environ 2001 traversées réutilisées en 5,870 s, soit environ 341/s. Ce
run valide pagination, checkpoints, Governor et reuse ; il n'est pas une mesure de latence MAGSAC
et n'en revendique ni médiane ni p95. Le RSS pic observé est 25 964 Kio pour le processus de test
complet. `MemAvailable` passe de 10 702 988 à 10 692 916 Kio ; `pswpin/pswpout` restent 0/0 ; en
fin de run, PSI avg10 vaut 0,34 % CPU, 0 % mémoire et 0 % I/O. Le chemin calculé reste couvert par
le vrai E2E MAGSAC Gate B et ses bornes, sans campagne scientifique répétée.

TSan couvre core, Task, sequencing et Governor (4/4), avec uniquement la suppression OpenCV
existante. Le build CPU-only couvre la suite normale (31/31). La suite normale ne contient ni
benchmark lourd ni stress. Le clean build Clang/Clang++ et la campagne normale finale passent
32/32 avec ORB Vulkan matériel sur Radeon 780M RADV PHOENIX.

## Out of scope

Tracks, model competition, classification planaire ou faible parallaxe, Essential, calibration,
pose, triangulation, bundle adjustment, SfM et Vulkan RANSAC.

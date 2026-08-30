# Precision Feature Pipeline v1A

## Contrat multipasse

**IMPLEMENTED.** Une image peut porter plusieurs `FeatureSet` immutables et
indépendants. PASS 0 conserve ORB v1 (`orb`, U8×32, Feature File v1,
`features.extract`) pour le Visual Index. PASS 1 ajoute SIFT v1 (`sift`,
F32×128, Feature File v2, `features.extract.sift`). PASS 1B applique RootSIFT
dans un Feature Set distinct (`rootsift`, version 1,
`features.extract.rootsift`). Une panne SIFT ne modifie jamais ORB et SIFT ne
dépend pas d'ORB pour être correct.

Le profil conceptuel `PRECISION_CLASSIC_V1` persiste toujours ses valeurs
effectives : 4096 features au plus, 3 couches par octave, seuil de contraste
0,02, seuil d'arête 10, sigma 1,6, grille 8×8 et quota 96 par cellule. Les
bornes sont respectivement 1..8192, 1..8, 0,001..0,2, 1..100, 0,5..3,
1..32 pour chaque dimension de grille et 1..8192 par cellule. Le fingerprint
SHA-256 encode les binary64 exacts en little-endian, tous les entiers, la
grille et le choix RootSIFT ; il ne dépend ni de la locale ni d'un JSON.

## Extraction et qualité spatiale

OpenCV 5 est appelé explicitement par `cv::SIFT::create(max_candidates,
octave_layers, contrast_threshold, edge_threshold, sigma, true)`. Le dernier
argument active le precise upscale d'OpenCV 5. L'appel `detectAndCompute` est
une frontière C++ non interruptible ; pause et annulation sont observées avant
et après. Les exceptions restent confinées à la façade C.

OpenCV peut dépasser légèrement `nfeatures` lorsque des réponses sont à
égalité. La sélection publiée reste strictement limitée par `max_features`; un
plafond défensif de 65 536 candidats rejette une sortie backend anormale avant
toute copie Lardon3D.

SIFT localise déjà les extrema à une position subpixel. Aucun `cornerSubPix`
supplémentaire n'est appliqué. CLAHE n'est pas activé ; il reste benchmark-only
et exigerait un fingerprint distinct s'il devenait un preprocessing production.

Les candidats sont classés de façon déterministe par réponse décroissante,
puis y, x et index OpenCV initial. Un quota par cellule est appliqué avant la
limite globale. Les coordonnées ne sont pas transformées. Une modification de
grille produit un nouveau fingerprint et donc de nouveaux indices immutables.
`occupied_cells`, `total_cells`, `coverage_ratio` et
`feature_density_per_megapixel` sont persistés dans ProjectDb ; une couverture
faible reste un diagnostic.

RootSIFT conserve exactement les keypoints SIFT. Chaque descriptor est
normalisé par sa somme L1 puis transformé composante par composante par racine
carrée. Un descriptor nul reste nul ; toute valeur non finie est rejetée. Le
benchmark test-only emploie L2, tandis qu'ORB reste dans son espace Hamming.

## Tâches, ressources et reprise

Une tâche traite une image et persiste ses paramètres dans
`sift_extract_tasks` avant enqueue. Une tâche READY identique devient un no-op
validé. Après crash, la registry statique reconstruit le même `task_id`; l'image
est recommencée, sans micro-checkpoint trompeur. Aucun Feature Set partiel ne
devient READY. Le SHA-256 de l'Image Asset géré est vérifié avant décodage.

L'estimation SIFT demande jusqu'à douze threads CPU. Le réglage OpenCV du
démarrage est une baseline/plafond sûre ; pour chaque séquence, l'unique
callback Queue applique temporairement le compte immuable admis dans `1..12`,
le vérifie puis restaure la baseline sur toute sortie. Une mutation process-wide
concurrente par plusieurs workers n'est pas supportée. La tâche réserve aussi
un slot IO, aucun GPU et environ 1,06 Gio structurels :
image décodée, pyramides OpenCV, candidats et descriptors. Le pic de lot est
zéro. Le Resource Governor admet donc le fan-out OpenCV exact ; la Queue
conserve un seul callback actif et ne superpose pas un second pool SIFT.

Le nombre de threads reste opérationnel et absent du fingerprint SIFT/RootSIFT.
La reprise accepte la forme CPU12 courante et normalise uniquement l'ancienne
forme CPU1 complète et exacte dans la copie privée restaurée. Elle ne publie
aucun checkpoint d'estimation seule ; une forme voisine est rejetée.
L'audit contrôlé OpenCV 5.0.0 à 1, 2, 4, 8 et 12 threads compare exactement le
count, l'ordre et les valeurs binary32 des champs keypoint persistés, les
octaves, toutes les lignes/octets F32×128 et le SHA-256 du Feature File v2.
Toutes les configurations convergent vers les mêmes octets durables. Le champ
OpenCV `class_id`, non transporté par le record FeatureSet gelé, n'est ni
persisté ni utilisé comme identité scientifique.

Le modèle n'est pas présenté comme un RSS mesuré : l'image grayscale atteint
100 MB à la limite post-décodage, le plafond défensif de candidats représente
au plus 32 Mio de descriptors F32 bruts, la sortie publiée au plus 4 Mio, et le
reste de la réservation couvre pyramides, buffers et overhead OpenCV. ORB
conserve son estimation historique de 576 Mio. Une API fiable de dimensions
multi-format avant décodage n'étant pas utilisée, le rejet 100 Mpx intervient
après `imread`.

La build OpenCV/TBB système n'est pas instrumentée par TSan et rapporte ses
propres accès de teardown TLS/`cv::Mat`. Les tests TSan suppriment uniquement
les frames des objets partagés `libopencv_core.so` et
`libopencv_features.so`; les frames Lardon3D et toutes les autres bibliothèques
restent contrôlées.

## Consolidation intra-image

PASS 3 crée un `feature_support_set` immutable pour une paire ORB/SIFT et un
rayon fingerprinté (4 px par défaut, 0,5..64). Une grille spatiale hachée trouve
le plus proche ORB non encore utilisé pour chaque SIFT. La complexité attendue
est O(N+M+candidats locaux), les entrées sont bornées à 8192 par Feature Set et
les égalités sont résolues par index. La position SIFT, déjà subpixel, devient
la représentante d'un groupe confirmé ; les observations isolées sont gardées.

Les tables normalisées `feature_support_groups` et
`feature_support_members` enregistrent `feature_set_id + feature_index` et un
`support_count` de 1 ou 2. La distance spatiale est persistée. L'API de ranking
transparente classe d'abord `support_count=2`, puis l'identité durable comme
tie-break; aucun score opaque n'est inventé. Aucun descriptor ORB n'est comparé, converti ou
fusionné avec un descriptor SIFT. Ces groupes sont intra-image : ce ne sont ni
des matches multivues, ni des tracks, ni une preuve géométrique.

## Durcissement et tests de consolidation

Le fichier `tests/test_precision_consolidation.c` contient 19 tests de
durcissement couvrant la consolidation ORB/SIFT. Ces tests sont uniquement des
tests unitaires ; aucun code de production n'est modifié.

**Couverture :**
- **zero-features** : consolidation avec zéro descripteur SIFT ou ORB.
- **rayon boundaries** : rayon à la borne inférieure (0,5 px) et supérieure
  (64 px).
- **grid boundary** : grille spatiale en bordure d'image.
- **support_count** : vérifie `support_count=1` (isolé) et `support_count=2`
  (paire confirmée).
- **mismatch** : aucun appariement lorsque les descripteurs sont trop éloignés.
- **large set** : performance avec un grand nombre de features (borné à 8192).
- **idempotence** : deux exécutions successives produisent le même résultat
  immutable.
- **concurrence** : accès concurrents à la consolidation sans corruption.
- **stress** : charge élevée pour valider les bornes mémoire et les
  invariants.
- **pagination** : lecture paginée des résultats de consolidation.
- **fingerprint** : reproductibilité du fingerprint SHA-256 des paramètres.
- **quality comparison** : comparaison de qualité entre zones consolidées et
  non consolidées.

Ces tests garantissent la robustesse des invariants de consolidation sans
modifier le comportement production. La validation ASan/UBSan et TSan est
recommandée pour les chemins concurrence et stress.

## Benchmark, stockage et frontières futures

`precision-features` construit une fixture et des homographies déterministes :
crop, rotations 5/15/30°, échelles 0,75/1,25/1,5, illumination, flou et faible
texture. Il rapporte count, couverture, répétabilité, localisation médiane/P90,
temps informatif et correction d'un BFMatcher/ratio test strictement test-only.
Ces résultats synthétiques ne prédisent pas la qualité sur de vraies photos.

À 4096 points, les descriptors SIFT occupent 2 Mio/image, plus environ 96 Kio
de keypoints et 176 octets de header. À 3700 images, cela représente 7,76 GB
(7,23 Gio) de descriptors et 8,12 GB (7,57 Gio) avec keypoints/header. Au
maximum 8192, les totaux sont 15,52 GB (14,45 Gio) de descriptors et 16,25 GB
(15,13 Gio) complets. RootSIFT en supplément double cette part SIFT ; 3700
images ne sont jamais chargées ensemble.

Le Visual Index v1 reste exclusivement ORB-LSH. Candidate Pair Generator,
matching production, vérification géométrique, tracks et SfM sont **PLANNED**.
ALIKED est **PLANNED / BLOCKED ON MODEL PROVENANCE + VALIDATED ONNX EXPORT** :
aucun code ALIKED, ONNX ou Python production n'appartient à v1A.

# Calibration Solver Preflight v1

**PASS — sélection d'un futur générateur externe d'évidence ; aucune
implémentation n'est introduite ici.** Ce document n'autorise aucune
modification de `CALIBRATION_SCIENCE_V1`, de `CALIBRATION_TOOLING_V1`, de
`L3DCALB1`, du Project DB ou de Sparse SfM. S21 historique reste exclu.

## Décision

La plus petite voie conforme est un exécutable C++ externe, versionné et
haché, construit contre **OpenCV 5.0.x** avec `core`, `imgproc`, `imgcodecs`,
`objdetect` et `calib`. Il n'est pas lié par Lardon3D, n'écrit pas le Project
DB et produit seulement un bundle d'évidence borné. Un adaptateur ultérieur
traduit ce bundle vers `calibration_tooling.h`; Lardon3D reste seul producteur
`L3DCALB1` et seul appelant de l'importeur.

Les alternatives évaluées sont : OpenCV ChArUco externe, retenu ; outils tiers
généralistes, non retenus faute de contrat public stable pour résidus point par
point et flags exacts ; solveur custom, hors scope car OpenCV fournit déjà la
détection, la calibration pinhole et les résidus nécessaires.

## API OpenCV et modèle

La cible est créée par `cv::aruco::CharucoBoard(cv::Size(9, 7), 0.030f,
0.021f, cv::aruco::getPredefinedDictionary(cv::aruco::DICT_5X5_100))`, puis
rendue par `generateImage`. La version OpenCV et `legacyPattern=false` sont
archivés. La détection emploie `cv::aruco::CharucoDetector::detectBoard` sans
matrice caméra au premier passage et archive coins, IDs, marqueurs et paramètres
de détection. `CharucoBoard::matchImagePoints` associe ID, pixel et point objet
en mètres. Une vue est explicitement rejetée, avec motif, si elle ne satisfait
pas Science v1.

Les coordonnées détectées `Point2f` ne sont pas le format de calcul publié :
l'exécutable les convertit une fois en `Point2d`; il reconstruit les points
objets `Point3d` depuis les IDs et la grille mesurée, plutôt que de propager les
`float` de commodité de la planche. `calibrateCamera` reçoit donc des matrices
`CV_64F`, et ses paramètres, poses et résidus sont conservés en `binary64`.

Le solveur appelle la surcharge étendue `cv::calibrateCamera` :

```text
flags = cv::CALIB_FIX_K3
criteria = COUNT | EPS, 500, DBL_EPSILON
cameraMatrix = Matx33d::eye()
distCoeffs = Mat::zeros(5, 1, CV_64F)
```

L'ordre OpenCV est `[k1,k2,p1,p2,k3]`; `CALIB_FIX_K3` fixe le dernier à zéro,
donc les quatre autres se mappent directement à Sparse SfM v1. `fx`, `fy`,
`cx`, `cy`, `k1`, `k2`, `p1` et `p2` restent libres. Sont interdits :
`CALIB_FIX_ASPECT_RATIO`, `CALIB_FIX_PRINCIPAL_POINT`,
`CALIB_FIX_FOCAL_LENGTH`, `CALIB_FIX_K1`, `CALIB_FIX_K2`,
`CALIB_ZERO_TANGENT_DIST`, `CALIB_FIX_TANGENT_DIST`,
`CALIB_RATIONAL_MODEL`, `CALIB_THIN_PRISM_MODEL`, `CALIB_TILTED_MODEL`, les
flags de fixage associés, fisheye, QR/LU, EXIF comme intrinsics et calibration
par image.

La surcharge étendue retourne RMS global et par vue. L'outil doit recalculer
avec `cv::projectPoints` chaque résidu par coin, maximum, fraction `>1 px` et
RMS avec les mêmes poses ; le succès du solveur seul n'est jamais suffisant.
Ces API emploient `fx,fy,cx,cy` en pixels et la même distorsion directe que le
modèle gelé : aucune conversion de modèle n'est permise.

## Entrée, sortie et déterminisme

L'entrée externe est un répertoire immuable d'originaux hachés, manifeste
d'état optique, cible/mesures et chaîne de décodage-orientation. Les vues sont
triées par SHA-256 source avant détection. Détection et solve sont deux étapes
matérialisées (`detection.json` et `solve.json`) d'un seul exécutable : IDs,
pixels subpixel, décisions et rejets sont ainsi vérifiables avant les trois
solves. Les fichiers ont ordre canonique, nombres `binary64` hexadécimaux,
tableaux ordonnés et limites déclarées.

La sortie archive nom/version/SHA de l'exécutable, OS/architecture, build
OpenCV et bibliothèques, configuration hachée, identité de session, vues et
motifs, points objet/image, résidus, poses diagnostic, huit paramètres,
supports, hold-out, trois répétitions et calculs des flags `0x01..0x08`. Les
images de campagne, postérieures à la calibration, sont ajoutées seulement par
l'adaptateur comme entrées `image_id`/SHA/dimensions dans l'ordre
`selected_execution` : elles ne sont jamais des entrées de solveur.

La preuve initiale est CPU1 : `cv::setNumThreads(1)`, `cv::setRNGSeed` fixé,
aucun travail parallèle, `OMP_NUM_THREADS=1`, `OPENBLAS_NUM_THREADS=1` et
`MKL_NUM_THREADS=1` lorsqu'applicables. `cv::getBuildInformation`, backend de
threads et environnement effectif sont archivés. Les trois exécutions sur même
hôte/architecture/octets doivent donner les mêmes huit `binary64`, décisions
et rapports canoniques : tout écart bloque Science v1.

## Équivalence de coordonnées et versionnement

La géométrie est : origine haut-gauche, `x` droite, `y` bas, pixels continus à
centres demi-entiers. Chaque image archive SHA, décodeur/version, EXIF,
dimensions avant/après et les équations `0°:(x,y)`, `90°:(H-y,x)`,
`180°:(W-x,H-y)`, `270°:(y,W-x)`. Vingt coins par vue repassent dans la
chaîne Feature Store ; chaque erreur est `<=0.01 px` et les dimensions orientées
doivent être identiques.

L'hôte de préflight fournit `opencv 5.0.0-9`. La production future épingle un
exécutable contre OpenCV 5.0.x avec SHA obligatoire ; le package Arch est bon
pour développement, pas l'unique identité de production. Sont archivés en
plus : compilateur, flags, build info, bibliothèques/SHA, OS, architecture et
politique CPU. Les quatre SHA `L3DCALB1` restent autoritaires.

## Prochaine tranche et première preuve physique

La prochaine tranche implémente uniquement l'exécutable externe et ses tests :
caméra pinhole/distorsion connues, projections ChArUco, bruit/outliers,
récupération, résidus, répétitions, hold-out, entrées invalides et bundle
byte-identique. Puis seulement : fabriquer/mesurer la cible, figer l'état
optique, acquérir au moins 40 vues, hacher, résoudre trois fois, vérifier
Science v1, passer le bundle à Calibration Tooling dans un projet dédié et
vérifier `CALIBRATION → READY`. La preuve s'arrête avant Sparse SfM ; S21 ne
participe jamais.

# Calibration Evidence Solver v1

Outil C++ externe de génération d'évidence pour `CALIBRATION_SCIENCE_V1`.
Il n'est pas une cible Meson, ne lie pas Lardon3D, ne lit ni n'écrit le Project
DB et ne produit pas `L3DCALB1`.

## Build local isolé

```sh
make
./calibration_evidence_solver --self-test
```

Le binaire attend OpenCV 5.x et OpenSSL. Le seul mode de production prévu est
CPU1; il force `cv::setNumThreads(1)` et une graine OpenCV fixe.

## Session v1 bornée et évidence pré-solve

Un fichier texte ASCII, sans commentaire ni espace dans les chemins, contient :

```text
L3DCAL_SESSION_V1
target <target-id> <generator-sha256> DICT_5X5_100 9 7 30 21
measurement <instrument-id> <resolution-mm> <10-real-square-mm-values>
white_border <measured-free-white-border-mm>
planarity PASS <planarity-evidence-sha256>
decoder <decoder-identity> <decoder-version>
optical_state <optical-state-sha256> <complete-state-token>
image <path> <64-hex-source> <orientation-0|90|180|270>
pre_solve <64-hex-source> <corner-rms-px>
clipping <64-hex-source> <fraction-in-target-box>
coordinate <64-hex-source> <decoder> <version> <orientation> <oriented-width> <oriented-height> <N>=20..48 <initial-max-dx> <initial-max-dy>
coordinate_point <64-hex-source> <coverage-label> <solver-x> <solver-y> <feature-x> <feature-y>
distance <64-hex-source> <physically-measured-metres> <band-0|1|2>
```

Il y a entre 1 et 4096 lignes `image`, chaque source SHA est unique, et chaque
image a exactement une preuve pré-solve, clipping et coordinate. Les dix
mesures réelles doivent être à 0.30 mm de 30.000 mm et leur étendue ne doit pas
dépasser 0.20 mm; l'instrument doit annoncer une résolution de 0.1 mm ou
meilleure. `white_border` archive la bordure blanche physique libre mesurée et
doit être d'au moins 30 mm; aucune valeur par défaut n'est inventée.
`planarity` est une attestation PASS hachée : Science v1 impose une planche
rigide plane mais ne fixe pas de seuil numérique que l'outil pourrait inventer. `optical_state` est un jeton atomique complet du manifeste d'état
optique; aucune valeur inconnue n'est acceptée par convention.

Les `coordinate_point` consomment exactement les `N` comparaisons annoncées.
Chaque point porte aussi, entre le SHA et les coordonnées, l'un des labels
`center`, `top`, `right`, `bottom`, `left`, `top_left`, `top_right`,
`bottom_left` ou `bottom_right`; les neuf catégories sont obligatoires. Chaque
vue porte une `distance` physique mesurée et l'une des trois bandes déclarées;
les poses estimées ne constituent jamais une preuve de distance d'acquisition.
Le solveur rejette une orientation différente de l'image, une dimension
orientée différente, moins de 20 points, ou une composante `dx`/`dy` supérieure
à 0.01 px. La déclaration lie donc SHA, décodeur/version, transformée
d'orientation, dimensions et mesures de coordonnées plutôt que d'émettre le
bit `0x08` par défaut.

Sur réussite, le solveur crée de manière atomique `<session>.bundle/`, qui est
immuable si déjà présent : `detection.json`, `solve.json`, `evidence.json` et
`producer.json`. Ce dernier lie le SHA-256 du binaire solveur en cours, le
SHA-256 de sa configuration canonique, le SHA-256 exact du manifeste de
session, la version/build OpenCV, la politique CPU1 et le SHA de l’état optique.

`detection.json` archive également les classifications effectivement utilisées
par Science v1 : région de cadre, bande de distance, hold-out, occupation,
angle, distance physique, masque de quadrants de cible et métriques
résiduelles par vue. Ces valeurs sont publiées comme évidence et ne doivent
pas être recalculées par le coordinator.
Les documents ont un ordre déterministe et encodent les valeurs faisant
autorité comme chaînes `hexfloat` binary64. `solve.json` archive les trois
sorties complètes (paramètres et poses); `evidence.json` archive les vecteurs
de résidu, RMSE par vue/global, maximum, fraction élevée, split fit/hold-out,
delta cinq directions et les quatre prédicats de `validation_flags`.

## Frontière numérique qualifiée

Le seul transport `float` est celui imposé par l'API OpenCV 5.0.x :
`CharucoDetector` fournit les pixels en `Point2f` et `calibrateCamera` reçoit
les points objet `Point3f`. Chaque valeur de validation est la promotion exacte
en `double` de ce transport ; les paramètres, poses, projections et résidus
publiés dans le rapport restent `binary64`. Le rapport archive le chemin de
conversion et ses bornes maximale image (pixels) et objet (mètres).

## Exécution

```sh
./calibration_evidence_solver --self-test
./calibration_evidence_solver --session session.l3dcal
make asan
```

`--self-test` est exclusivement synthétique : il crée 60 vues d'une caméra
connue, quantifie une seule fois les observations `binary64` vers le transport
OpenCV `binary32`, puis vérifie récupération, résidus indépendants, split
fit/hold-out et répétabilité CPU1. Il ne représente jamais une calibration
physique.

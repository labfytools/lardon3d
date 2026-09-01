# Calibration Science v1

**PASS / FROZEN — protocole scientifique pour les futures acquisitions.** Ce
document définit le contrat complet qui produit l'évidence d'une calibration
connue pour Sparse SfM v1. Il ne définit ni un solveur, ni une interface, ni
une tâche, ni un import automatique. Il n'autorise pas à rétro-calibrer une
acquisition historique dont l'état optique n'est pas prouvé.

En particulier, `S21_CALIBRATION_PREFLIGHT=BLOCKED` reste définitif pour la
campagne S21 Engine Bay historique : une session physique prise ultérieurement
ne peut pas attester l'état de prise de vue historique. Ce document s'applique
seulement à une campagne future qui respecte ce protocole depuis le début.

`HISTORICAL_S21_RETRO_CALIBRATION=FORBIDDEN`.

## 1. Objet, frontières et modèle gelé

Une calibration Science v1 produit un modèle intrinsèque pour une *géométrie de
représentation* donnée. Elle ne produit ni pose externe, ni point 3D, ni Track,
ni Match, ni résultat de reconstruction. Sparse SfM v1 conserve exactement le
modèle pinhole connu suivant, en `binary64` :

```text
K = [ fx  0  cx ]
    [  0 fy  cy ]
    [  0  0   1 ]

r2 = x*x + y*y
x_d = x * (1 + k1*r2 + k2*r2*r2) + 2*p1*x*y + p2*(r2 + 2*x*x)
y_d = y * (1 + k1*r2 + k2*r2*r2) + p1*(r2 + 2*y*y) + 2*p2*x*y
u = fx*x_d + cx
v = fy*y_d + cy
```

Les huit paramètres persistés sont `fx`, `fy`, `cx`, `cy`, `k1`, `k2`, `p1`
et `p2`. Skew est toujours nul. Les modèles à coefficients supplémentaires,
rationnels ou thin-prism sont incompatibles : ils sont rejetés, jamais
tronqués. EXIF (notamment `FocalLength`) n'est qu'un indice de regroupement ou
d'initialisation du solveur ; il ne peut jamais devenir un paramètre publié.

La calibration s'applique aux coordonnées pixels après la normalisation
d'orientation et avant l'extraction de features, exactement dans le repère
consommé par le Feature Store : origine au coin supérieur gauche, `x` vers la
droite, `y` vers le bas, coordonnées continues en pixels. Le centre d'un pixel
est donc au demi-entier usuel ; aucun décalage caché de demi-pixel n'est permis.
`width,height` sont les dimensions décodées/orientées de cette géométrie, pas
seulement celles du flux JPEG encodé.

Ce contrat n'autorise aucun changement de Sparse SfM v1, des observations, du
Feature Store ni du format `L3DCALB1` v1.

## 2. Cible physique canonique : ChArUco planaire V1

La cible canonique est une planche **ChArUco rectangulaire 9 x 7 cases**,
dictionnaire `DICT_5X5_100`, avec cases de **30.000 mm** et marqueurs de
**21.000 mm**. La géométrie active fait donc 270.000 mm x 210.000 mm ; elle
fournit 48 coins de damier interpolés, plus les identifiants non ambigus des
marqueurs. Le rectangle non carré évite les ambiguïtés de rotation des damiers
carrés ; les identifiants rendent les vues partielles identifiables.

Ce choix est scientifique, non une commodité d'implémentation : les coins
ChArUco combinent l'identification des marqueurs et la localisation subpixel
des coins de damier ; la documentation OpenCV les recommande explicitement aux
marqueurs ArUco seuls lorsque la précision de calibration compte. Voir
[Detection of ChArUco Boards](https://docs.opencv.org/5.0/tutorials/objdetect/charuco_detection/charuco_detection.html).
La documentation de motifs OpenCV fournit également un exemple ChArUco
physique à 30 mm et rappelle que plus de features et des éléments plus grands
réduisent l'incertitude de détection ; les dimensions ci-dessus fixent ces
choix de façon reproductible. Voir
[Create Calibration Pattern](https://docs.opencv.org/5.0/tutorials/calib3d/camera_calibration_pattern/camera_calibration_pattern.html).

La cible doit respecter toutes les conditions suivantes :

- le fichier générateur de la planche, le dictionnaire, le nombre de cases et
  les longueurs nominales sont archivés et hachés ;
- les longueurs réelles de dix cases réparties (cinq horizontales, cinq
  verticales) sont mesurées avec une résolution de 0.1 mm ou meilleure ; leur
  écart maximal à 30.000 mm ne dépasse pas 0.30 mm et leur étendue ne dépasse
  pas 0.20 mm ;
- les mesures et l'identifiant de l'instrument sont archivés ; les seuils de
  1 % et 0.67 % bornent respectivement l'erreur d'échelle et une déformation
  différentielle qui rendrait le plan objet déclaré faux ;
- l'impression est noire/blanche, mate, à contraste uniforme, sans mise à
  l'échelle par l'imprimeur ; elle est collée ou imprimée sur un support rigide
  plan ;
- une bordure blanche libre d'au moins une largeur de case (30 mm) entoure la
  géométrie active ; elle protège les marqueurs du recadrage et des ombres ;
- toute planche gondolée, brillante, rayée, tachée, dont un marqueur est
  illisible ou dont les mesures échouent est invalide.

Les tolérances de fabrication ne sont pas des tolérances de caméra : elles
empêchent que la géométrie objet fournie au solveur contredise la cible
physique. Aucune correction logicielle de planche n'est autorisée.

## 3. État optique et clé de regroupement

Une calibration n'est réutilisable que pour des images qui ont la même clé
scientifique suivante. Une valeur inconnue est une incompatibilité, pas une
valeur par défaut.

| Facteur | Règle Science v1 |
|---|---|
| Corps/module physique | **MUST match** ; un autre module, appareil ou capteur impose un groupe distinct. |
| Objectif | **MUST match** ; changement d'objectif ou adaptateur impose un groupe distinct. |
| Focale/zoom optique | **MUST match** à l'état explicitement enregistré ; un zoom est calibré séparément à chaque cran verrouillé. |
| Zoom numérique / crop capteur | **Unsupported** sauf profil explicite de crop, dimensions et preuve de coordonnées ; sinon groupe impossible. |
| Résolution et aspect ratio décodés | **MUST match**. |
| Pipeline de correction optique interne | **MUST match** ; changement de mode ou état inconnu impose groupe distinct. |
| Stabilisation | **MUST match** ; stabilisation électronique, recadrage stabilisé, ou état inconnu rend le groupe non calibrable. OIS purement optique peut varier seulement si une étude de répétabilité conforme à §7 le démontre. |
| Focus | **MUST match**. Le focus est verrouillé à une distance déclarée ; autofocus est non pris en charge par Science v1, sauf si chaque état discret est verrouillé et validé comme groupe indépendant. |
| Ouverture | **MAY vary with proof** pour un objectif à diaphragme, seulement si les essais de répétabilité et les seuils de §7 passent à chaque ouverture déclarée ; sinon une ouverture impose un groupe. Pour un module téléphone à ouverture fixe, elle est enregistrée mais ne crée pas seule un groupe. |
| Orientation EXIF | N'identifie pas le matériel, mais **MUST be transformed explicitly** vers le repère Feature Store ; les représentations orientées distinctes nécessitent des paramètres publiés compatibles avec leurs coordonnées. |
| Resize/crop après capture | **Unsupported** sauf transformation déterministe, hachée, exécutée avant calibration et features, avec preuve de §8. |
| Couleur, balance des blancs, compression JPEG | **Irrelevant** pour le modèle géométrique après preuve que les coordonnées de l'image décodée sont inchangées. |

La clé de groupe conservée dans l'évidence est donc :

```text
body_or_module_identity
objective_identity
focal_or_locked_zoom_state
focus_mode_and_locked_distance
stabilization_and_computational_mode
encoded_format_and_decode_pipeline_identity
oriented_width_height
crop_and_resize_transform_identity
in_camera_correction_pipeline_identity
```

Une calibration identique peut être liée à plusieurs `image_id` seulement si
la clé complète, les huit paramètres publiés, `provenance_kind` et le même
`provenance_fingerprint` (pour un import : le SHA-256 du même artifact
`L3DCALB1`) sont identiques. Cette règle exige donc l'égalité de l'identité
scientifique complète de calibration, pas seulement de ses intrinsics. Le scope
reste une relation par image : il doit couvrir toutes les images sélectionnées,
même lorsqu'elles réutilisent une même calibration content-addressed.

## 4. Protocole physique d'acquisition

Le protocole vaut pour **un seul groupe optique**. Une variation de la clé de
§3 impose une session distincte.

1. Avant la session, verrouiller et noter le module/objectif, focale ou cran de
   zoom, mise au point, stabilisation, mode HDR/computationnel, résolution,
   ratio, format et orientation de sortie. Les modes portrait, night, HDR
   multi-image, super-résolution, correction de perspective, beauty filter,
   bokeh, panorama et tout mode dont le pipeline géométrique n'est pas déclaré
   sont interdits.
2. Désactiver l'EIS et tout recadrage stabilisé. Désactiver l'autofocus après
   verrouillage à la distance de travail déclarée. Si l'appareil ne permet pas
   ce contrôle ou cette observation, ce groupe ne satisfait pas Science v1.
3. Capturer **au moins 40 vues acceptées**, viser **60 vues acceptées**, sans
   réutiliser de rafale quasi identique. Le minimum est quatre fois les dix
   bonnes vues pratiques indiquées par la documentation de calibration OpenCV :
   ce surplus n'est pas une précision statistique prétendue, il réserve des
   vues pour les quatre zones de champ, les quatre inclinaisons et les
   validations hold-out ci-dessous.
4. Dans chaque quart de l'image (haut-gauche, haut-droit, bas-gauche,
   bas-droit), accepter au minimum six vues dont le centre de la cible est dans
   ce quart ; au minimum huit vues supplémentaires ont le centre dans la zone
   centrale. Une vue compte pour un seul compartiment. Cette règle empêche une
   calibration centralisée de prétendre mesurer la distorsion de bord.
5. La cible doit occuper entre **20 % et 80 %** de la plus petite dimension de
   l'image. Sous 20 %, les 30 mm et les coins subpixel deviennent trop petits
   face à la quantification ; au-dessus de 80 %, la cible ne contraint plus
   utilement l'ensemble du champ. Cette bande force à la fois précision locale
   et couverture globale.
6. Au moins 24 vues présentent une inclinaison de normale de plan de **20° à
   60°**, avec au moins six vues dans chacune des classes 20–35°, 35–50° et
   50–60°. Les vues frontales peuvent compléter la couverture mais ne comptent
   pas pour cette diversité. Les angles évitent le cas plan quasi frontal,
   insuffisant pour séparer focale, point principal et distorsion.
7. Utiliser au moins trois bandes de distance, dont la distance médiane est
   proche de la distance de travail de la future campagne ; chaque bande doit
   fournir huit vues acceptées. Les bandes sont documentées en mètres mesurés,
   avec un rapport entre distance maximale et minimale d'au moins 1.5. La
   variation borne la corrélation entre position et paramètres intrinsèques.
8. Chaque vue acceptée contient au moins **16 coins ChArUco interpolés**,
   répartis dans au moins trois quadrants de la boîte englobante de la cible.
   Une occlusion est admissible seulement si elle ne masque ni ne déforme les
   coins retenus ; la cible est sinon rejetée pour cette vue. Les identifiants
   ChArUco permettent cette sélection sans correspondance ambiguë.
9. Les images doivent être nettes au niveau des coins ; une vue est rejetée si
   le solveur/validateur ne peut pas localiser les coins à une incertitude
   subpixel finie ou si son erreur RMS de coins avant solve est supérieure à
   **0.25 px**. Ce seuil est un garde-fou de mesure, inférieur au budget global
   de 0.50 px de §6 : une vue dont la détection est déjà instable ne doit pas
   être sauvée par l'ajustement global.
10. Éclairage diffus, sans reflets saturés, ombres dures ni scintillement ;
    exposition qui ne clippe aucun carré ou marqueur utilisé. Une vue est
    rejetée si plus de **1 %** de ses pixels dans la boîte de la cible sont à
    0 ou 255 après décodage. Cette limite vise le signal de localisation, pas
    une métrique esthétique générale.
11. Le fichier source est conservé sans rotation, resize, crop, recompression,
    métadonnée réécrite ou conversion après capture. RAW est accepté seulement
    si le développement vers la représentation géométrique est déterministe,
    versionné et identique aux futures images ; JPEG source est accepté
    lorsqu'il est cette représentation. Toute transformation doit être la même
    avant calibration et features, et doit posséder le manifeste de §8.

Les seuils de diversité sont des **HARD REJECTS** : ils sont des contraintes
d'observabilité, non des recommandations de confort. La recommandation de base
de multiples vues à différentes positions est établie dans
[Camera calibration With OpenCV](https://docs.opencv.org/5.0/tutorials/calib3d/camera_calibration/camera_calibration.html).

## 5. Contrat de solveur

Science v1 choisit **B : contrat d'évidence indépendant du solveur**. Un seul
produit ou une seule version de bibliothèque ne doit pas devenir une identité
scientifique implicite. Un solveur est conforme seulement s'il publie son
exécutable, sa version, son SHA-256, sa configuration complète et les sorties
décrites en §9. Un futur outil peut employer une implémentation conforme ; cela
n'ajoute pas OpenCV comme dépendance Lardon3D.

Le solveur doit :

- prendre les coordonnées physiques mesurées de la planche et les coordonnées
  image subpixel des coins identifiés ;
- estimer exactement les huit paramètres autorisés et une pose par vue ;
- conserver `binary64` pour la géométrie de calcul, l'optimisation, les
  résidus, les projections indépendantes et les rapports faisant autorité,
  sous la seule exception de transport d'observations définie ci-dessous ;
- ne pas fixer `fx=fy`, le point principal, ni les coefficients de distorsion ;
- ne pas optimiser d'intrinsics par image dans un même groupe ;
- détecter les systèmes de rang insuffisant, valeurs non finies, paramètres
  hors domaine, échec numérique et absence de convergence ;
- rejeter, documenter et exclure explicitement une vue aberrante plutôt que
  l'effacer silencieusement ;
- reporter les résidus 2D par coin et les métriques par vue ;
- produire les paramètres sous forme `binary64` directement mappables aux
  huit champs `L3DCALB1` ;
- exécuter avec graine déterministe ou sans aléa et produire les mêmes octets
  de rapport lors de répétitions identiques.

### Exception contrôlée : transport d'observations `binary32`

Les observations 2D image et 3D objet **passées à une API de solveur externe
qualifiée** peuvent être en IEEE-754 `binary32` seulement lorsque toutes les
conditions suivantes sont satisfaites :

1. l'API qualifiée exige cette représentation ;
2. la mesure ou représentation source n'est elle-même nativement pas plus
   précise que `binary32` ;
3. la conversion est faite exactement une fois, de façon déterministe ;
4. aucune quantification supplémentaire de l'observation ne survient après
   cette conversion ;
5. le bundle de provenance enregistre le chemin de conversion ;
6. le bundle mesure et rapporte l'erreur maximale de conversion par
   coordonnée, dans l'unité de la coordonnée concernée ;
7. pour chaque coordonnée image, cette borne mesurée est strictement
   inférieure au seuil gelé d'équivalence de coordonnées, soit `0.01 px` ;
   pour chaque coordonnée objet, le bundle rapporte la borne en unité physique
   de la cible et la représentation source qui justifie l'exception.

Cette exception est une frontière de transport, non une baisse de précision
scientifique. Elle ne s'applique jamais aux paramètres publiés
`fx, fy, cx, cy, k1, k2, p1, p2`, à la sortie faisant autorité
`cameraMatrix/distCoeffs`, aux poses archivées, aux projections et vecteurs
de résidu indépendants, aux RMSE par vue et global, au résidu maximal, à la
fraction de résidus élevés, aux métriques fit/hold-out, à
`maximum_parameter_delta`, à la validation d'équivalence de coordonnées,
aux décisions finales de validation ni aux champs numériques `L3DCALB1`.
Toutes ces quantités restent en IEEE-754 `binary64`.

La justification qualifiée pour l'échelle d'image prise en charge est la
suivante : à `8192 px`, l'arrondi `binary32` est borné à environ
`0.0005 px` par composante, très en dessous de `0.01 px`. La détection
ChArUco qualifiée produit nativement des `Point2f` ; promouvoir ces mesures
en `binary64` ne restaure donc aucune précision de mesure. Chaque session
doit néanmoins rapporter sa borne réellement mesurée : les seuils de §4, §6,
§7 et §8, ainsi que tous leurs calculs de validation en `binary64`, restent
inchangés.

La convergence est valide seulement si la terminaison déclarée par le solveur
est une convergence réussie, tous les paramètres/résidus sont finis, et tous
les contrôles de §6–§8 passent. Une baisse de coût sans convergence ne suffit
pas.

Les covariance/incertitudes ne sont pas un champ persistant Sparse v1 et ne
sont donc pas une condition de publication. Le solveur peut les archiver comme
diagnostic ; il ne peut pas les substituer aux seuils objectifs du contrat.

## 6. Acceptation quantitative

Les métriques utilisent les résidus euclidiens en pixels dans le repère
Feature Store. Les seuils ci-dessous sont normatifs. Ils sont volontairement
plus stricts que le seuil Sparse générique ne pourrait l'être : une calibration
est une mesure d'entrée commune à tout le pipeline.

| Mesure | Seuil | Décision | Justification / sens de l'échec |
|---|---:|---|---|
| Vues acceptées | `>= 40` | HARD REJECT | Quatre fois le minimum pratique de dix vues indiqué par OpenCV ; permet diversité et hold-out. Moins de vues rend les tests de couverture non satisfaisables. |
| Coins acceptés | `>= 1600` | HARD REJECT | 40 vues × 40 coins moyens : la valeur exige une information répartie, au-delà du minimum local de 16. |
| Occupation de cible | `20 %..80 %` | HARD REJECT par vue | Protège localisation subpixel et couverture de champ. |
| Couverture des quatre quarts | `>= 6` vues/quart | HARD REJECT | Empêche l'extrapolation de bord depuis le centre. |
| Diversité angulaire | §4.6 | HARD REJECT | Rejette la géométrie planaire quasi frontale dégénérée. |
| Diversité de distance | §4.7 | HARD REJECT | Réduit les corrélations intrinsèques/pose. |
| RMSE globale | `<= 0.50 px` | HARD REJECT | Budget de précision inférieur au pixel ; au-delà, le modèle v1 ou l'état optique ne représente pas la mesure. |
| RMSE par vue | `<= 0.75 px` | HARD REJECT | Une bonne moyenne ne doit pas masquer un sous-ensemble mal modélisé. |
| Résidu individuel maximal | `<= 1.50 px` | HARD REJECT | Trois fois le budget global : au-delà, correspondance/corner ou modèle incompatible. |
| Fraction de résidus `> 1.0 px` | `<= 1 %` | HARD REJECT | Empêche qu'une longue queue soit absorbée par une moyenne basse. |
| Détection de coin pré-solve | `<= 0.25 px RMS` | HARD REJECT par vue | Garantit que le bruit de mesure ne consomme pas à lui seul le budget de modèle. |
| Répétition même jeu | §7, maximum de projection `<= 0.10 px` sur les cinq rayons canoniques | HARD REJECT | Le résultat doit être stable à une fraction du budget global. |
| Hold-out | RMSE `<= 0.75 px`, max `<= 1.50 px` | HARD REJECT | Détecte une calibration qui ne généralise qu'aux vues ajustées. |

Le seuil 0.50 px n'est pas présenté comme une loi universelle de la vision :
c'est une politique v1 fondée sur la localisation ChArUco subpixel et une
exigence stricte de cohérence pixel à pixel. Le seuil par vue de 0.75 px et le
maximum de 1.50 px sont respectivement 1.5× et 3× ce budget ; leurs rôles sont
de détecter l'hétérogénéité et les résidus grossiers. Tout échec signifie que
le groupe est incompatible avec le modèle v1, le protocole ou les données ; il
ne se corrige jamais par assouplissement des seuils.

Les avertissements, sans publication bloquée, sont seulement : RMSE globale
dans `(0.40, 0.50] px`, ou nombre de vues dans `[40, 59]`. Ils exigent une note
de revue dans l'évidence ; ils ne modifient pas les règles HARD REJECT.

## 7. Répétabilité et sens des quatre flags

L'évidence de validation doit rendre `validation_flags=0x0f` objectif :

1. **Convergence (`0x01`)** : le solveur a une terminaison réussie, tous les
   paramètres et résidus sont finis, et §6 est satisfait.
2. **Support non dégénéré (`0x02`)** : le jeu satisfait exactement les règles
   de nombre de vues, coins, quarts, occupation, angles et distances de §4 et
   §6. Un solveur qui annonce simplement un rang non nul ne peut pas remplacer
   ces vérifications de protocole.
3. **Stabilité déterministe (`0x04`)** : exécuter trois fois le solveur sur les
   mêmes octets, même configuration et même ordre donne les mêmes huit valeurs
   `binary64` et les mêmes listes de vues acceptées/rejetées. Le jeu complet
   satisfait §4 et §6. Pour le contrôle de généralisation, trier les vues par
   `(quart_de_centre, bande_distance, classe_angle, sha256_source)` et affecter
   chaque cinquième vue de chaque strate au hold-out ; les autres forment le
   fit. Le fit a au moins 32 vues et au moins quatre vues dans chacun des quatre
   quarts ; le hold-out a au moins huit vues. Ces sous-jeux n'ont pas à répéter
   le minimum global de 40 vues, qui reste une règle du jeu complet. Ajuster le
   fit, évaluer le hold-out avec les seuils hold-out de §6, puis comparer le
   fit et le jeu complet par la métrique ci-dessous.

   La métrique unique de stabilité est le `maximum_parameter_delta` gelé dans
   chaque entrée `L3DCALB1`, exprimé en pixels : pour chacune des cinq directions
   normalisées `[(0,0),(-0.7,-0.7),(0.7,-0.7),(-0.7,0.7),(0.7,0.7)]`, appliquer
   le modèle direct de §1 dans chaque calibration, calculer la norme L2 des deux
   pixels projetés, puis prendre le maximum des cinq normes. Ce maximum doit
   être `<= 0.10 px`. Ainsi le champ ne compare pas naïvement des coefficients
   de distorsion et son unité reste compatible avec le champ artifact existant.
4. **Équivalence de coordonnées (`0x08`)** : le manifeste et le test de §8
   passent pour chaque représentation orientée du groupe.

Les répétitions utilisent les mêmes images sans modification ; une session
physique distincte est une validation de surveillance recommandée avant chaque
campagne majeure, mais n'est pas requise pour publier un groupe. Si elle est
réalisée, elle doit passer les mêmes seuils et la comparaison de 0.10 px.

## 8. Preuve obligatoire d'équivalence de coordonnées

Avant l'import, le producteur d'artifact doit créer un manifeste par
représentation qui établit la transformation exacte :

```text
octets source hachés
→ décodeur identifié/versionné
→ application EXIF documentée
→ orientation normalisée
→ crop/resize déterministe, s'il existe
→ image géométrique de calibration
→ convention Feature Store `image_width,image_height` et repère de keypoints
```

Le test reproductible est le suivant :

1. prendre au moins 20 coins ChArUco répartis sur centre, quatre bords et
   quatre coins de chaque **vue de calibration** de validation ;
2. pour chaque coin, encoder son pixel solver et son pixel après la même chaîne
   de décodage/orientation que celle qui définit la géométrie Feature Store ;
3. vérifier `abs(dx) <= 0.01 px` et `abs(dy) <= 0.01 px` pour chaque point ;
4. vérifier l'égalité exacte de `width,height` entre manifeste et l'image
   géométrique décodée/orientée ;
5. vérifier SHA-256 de la représentation source. Après publication des
   Features de campagne, vérifier séparément que chaque Feature File porte ces
   mêmes `image_width,image_height` et le SHA-256 de l'image source ; aucun hash
   de Feature File ne fait partie de `L3DCALB1` v1 ;
6. répéter pour les transformations EXIF présentes : identité, 90°, 180° et
   270°. Toute orientation non testée ne peut pas appartenir au scope.

Les rotations sont transformées dans le plan image avant calibration : le
solveur ne calibre jamais des coordonnées JPEG encodées non orientées alors que
les features sont orientés. Row stride/padding sont hors sujet après décodage ;
la conversion de couleur est également hors sujet si, et seulement si, elle ne
change ni dimensions ni coordonnées. Crop, resize et réencodage ne sont admis
que lorsque leur transformée affine/discrète exacte fait partie du manifeste,
est appliquée aux images de calibration et de campagne, et réussit ce test.

Cette procédure est la seule base autorisée pour `0x08`; une simple inspection
visuelle, un EXIF Orientation, ou une égalité de ratio d'image ne suffisent pas.

## 9. Bundle de provenance et L3DCALB1

Le producteur conserve un bundle immuable, puis place ses quatre SHA-256 dans
`L3DCALB1` v1. Les contenus minimaux sont :

| Digest L3DCALB1 | Contenu canonique haché |
|---|---|
| `solver_executable_sha256` | binaire exact du solveur, version, plateforme et SHA-256 du binaire. |
| `solver_configuration_sha256` | modèle huit paramètres, paramètres numériques, règles d'outliers, ordre des entrées, graines et version de dictionnaire ChArUco. |
| `initialization_evidence_sha256` | manifeste de cible, mesures physiques, identifiant de planche, état optique de groupe, liste/sha des vues et initialisation effectivement utilisée. |
| `validation_evidence_sha256` | résidus par coin et vue, vues rejetées/motifs, contrôles §4–§8, répétitions, partitions, projections de comparaison, chemin/bornes de conversion d'observations si §5 s'applique, et résultat flag par flag. |

Le bundle doit en outre archiver : fichier générateur de cible et son SHA,
photographies originales de calibration et SHA, snapshots EXIF complets,
manifeste de l'état optique, dimensions décodées/orientées, manifeste des
transformations de coordonnées, stdout/stderr du solveur, rapport final des
huit paramètres et liste explicite de tout fichier refusé. Les diagnostics de
poses peuvent être archivés, mais ne franchissent jamais la frontière
Lardon3D scientifique.

Chaque entrée `L3DCALB1` reste une entrée par `image_id` sélectionné :
`image_id`, SHA de représentation, dimensions orientées, huit paramètres,
supports, RMSE, delta maximal et flags. L'importeur existant vérifie les SHA,
le format, les limites et l'appartenance exacte au snapshot, puis crée/réutilise
les calibrations, le scope et l'attachement idempotent. Ce contrat ne le
modifie pas.

## 10. Workflow de campagne et invalidation

Le protocole physique de calibration doit précéder l'acquisition de campagne,
mais l'artifact lié à `image_id` ne peut être produit qu'après que les images
futures ont une identité et une représentation durable. Le workflow canonique
est donc :

```text
qualifier et verrouiller l'état optique
→ mesurer et archiver la planche ChArUco
→ capturer la session physique de calibration
→ acquérir les images de campagne avec le même état documenté
→ créer les identités/représentations image durables
→ Features (puis contrôle dimensions/SHA des Feature Files)
→ solves + répétitions + preuve de coordonnées sur cette géométrie
→ bundle immuable + L3DCALB1 lié aux image_id sélectionnés
→ import de calibration et scope immutable
→ Matching → GV → Tracks → Sparse SfM
```

Une seconde session juste après est recommandée lorsque l'équipement le permet ;
elle est requise après toute mise à jour d'OS/appareil photo, changement de
module/objectif, chute/réparation, changement de focale/zoom/focus, changement
de résolution, ratio, HDR/traitement, correction optique, stabilisation,
crop/resize ou décodage. L'image de campagne dont l'état ne peut pas être lié à
une clé §3 reste `CALIBRATION_UNAVAILABLE`.

Les appareils à objectifs interchangeables, fixes, téléphones et sorties HDMI
peuvent tous utiliser Science v1 sans changer Sparse SfM v1. La différence est
leur clé : objectif/focale/ouverture pour un appareil interchangeable ; module,
focus, EIS et pipeline computationnel pour téléphone ; capteur, objectif,
mode de capture et transformation HDMI pour une chaîne HDMI. Une chaîne HDMI
doit calibrer le flux effectivement décodé, pas le capteur théorique.

## 11. Conditions d'invalidation normative

Un groupe est inutilisable pour Known Calibration Sparse SfM si l'une de ces
conditions est vraie : état optique inconnu, zoom/crop inconnu, autofocus non
verrouillé, EIS ou mode computationnel non maîtrisé, format/résolution/ratio
différent, module/objectif changé, correction interne différente, target non
mesurable ou non plane, vue insuffisante/floue/écrêtée, échec d'un seuil §6,
échec de stabilité §7, échec d'équivalence §8, provenance/hash incomplet, ou
absence de binding exact image→calibration dans le scope.

Il n'existe aucune exception « même appareil » ou « focale EXIF identique ».

## 12. Statut et suite

`CALIBRATION_SCIENCE_V1=PASS/FROZEN`. La prochaine tranche autorisée est un
producteur borné d'évidence/`L3DCALB1` qui applique exactement ce contrat ; il
ne doit ni modifier l'importeur ni lancer Sparse SfM. Une campagne historique
ne peut pas devenir éligible rétrospectivement.

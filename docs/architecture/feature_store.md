# Feature Store v1

## Rôle et modèle

**IMPLEMENTED.** Le Feature Store est la mémoire visuelle locale persistante.
Un `FeatureSet` logique appartient à une `image_id` et identifie exactement
`orb` version 1 plus une configuration canonique. Il référence un
`FeatureAsset` physique immutable. Deux images logiques portant le même contenu
et la même configuration gardent deux `feature_set_id`, mais partagent le même
asset lorsque leurs octets sont identiques. Les futurs matches référenceront
`feature_set_id + feature_index`; ces deux valeurs sont immuables après
publication.

`feature_set_id` et `feature_asset_id` sont des identités SQLite
`AUTOINCREMENT`. Une identité validée n'est jamais réattribuée. SQLite ne
contient que les relations, empreintes, tailles et paramètres ; les tableaux de
points et descripteurs restent hors DB.

## Extracteur et configuration

L'extracteur production est ORB d'OpenCV 5, derrière une façade C. Aucune
exception C++ ni `cv::Mat` ne traverse cette frontière. La registry statique
expose `features.extract`, version 1. Une tâche traite exactement une image.

La configuration v1 contient trois `uint32` : `max_features` (1..8192),
`pyramid_levels` (1..16) et `fast_threshold` (1..255). Son fingerprint SHA-256
porte sur les 24 octets canoniques `L3DORBP1`, version et trois entiers
little-endian. Il ne dépend ni du padding, ni de la locale. Le contrat ORB v1
produit 32 octets binaires par point.

OpenCV n'est pas configuré via un état global par Lardon3D. Le scheduler actuel
n'a qu'un worker ; OpenCV peut néanmoins employer son backend parallèle
interne. Le contrôle fin de ce parallélisme devra précéder les futurs pools de
workers.

La limite de 100 000 000 pixels est vérifiée après `cv::imread` : l'API utilisée
ne fournit pas de sonde de dimensions multi-format fiable sans décodage. Le pic
mémoire du décodage peut donc précéder le rejet. Lardon3D ne revendique pas de
bornage pré-décodage et n'embarque pas un parseur JPEG/PNG parallèle.

## Feature File v1

Le fichier est little-endian et exige IEEE-754 binary32. Sa taille maximale est
16 Mio et son nombre maximal de points 8192.

| Offset | Taille | Champ |
|---:|---:|---|
| 0 | 8 | magic `L3DFEAT\0` |
| 8 | 4 | format version = 1 |
| 12 | 4 | header size = 160 |
| 16 | 4 | feature count |
| 20 | 4 | descriptor dimension = 32 |
| 24 | 4 | descriptor type = U8 |
| 28 | 4 | keypoint record size = 24 |
| 32,36 | 4+4 | largeur, hauteur décodées |
| 40,48,56 | 8+8+8 | offsets keypoints/descriptors, taille totale |
| 64 | 32 | SHA-256 de l'asset image source |
| 96 | 32 | fingerprint paramètres |
| 128 | 16 | `orb\0` puis zéros réservés |
| 144 | 4 | extractor version = 1 |
| 148 | 12 | réservés, zéro obligatoire |

Chaque keypoint contient six mots de 32 bits : `x`, `y`, `size`, orientation,
response en binary32, puis octave signé. `x/y` sont en pixels de l'image telle
que décodée par OpenCV, origine en haut à gauche. `size` est le diamètre du
voisinage en pixels. L'orientation est en degrés dans `[0,360)`. L'entrée `i`
du bloc keypoints correspond exactement aux 32 octets du descriptor `i`.

Le validateur contrôle magic, version, réservés, type, dimension, bornes,
offsets, taille exacte, multiplications, SHA-256 externe et cohérence DB. Une
version future est distinguée d'une corruption lorsque le fichier et son record
DB sont cohérents. Le reader utilise `pread`, accepte au plus 256 éléments par
appel et ne charge jamais le fichier entier.

Le chemin DB est validé sous sa forme canonique exacte dérivée du SHA-256 avant
toute ouverture. Une absence retourne `NOT_FOUND`; troncature, hash divergent,
header ou métadonnées DB divergents retournent `CORRUPT`. Une version future
retourne distinctement `UNSUPPORTED_VERSION`.

## Publication et persistance

Layout : `assets/features/<2 hex>/<sha256 complet lowercase>`. Le SHA-256 porte
sur le Feature File complet. Le protocole est : temporaire local, écriture,
`fsync`, hash, `link` atomique sans écrasement, validation complète lors d'une
adoption concurrente, `fsync` du répertoire, puis transaction SQLite. Un échec
du dernier `fsync` est enregistré `PUBLISHED_NOT_DURABLE`. Un échec SQLite après
publication laisse un fichier orphelin et aucune ligne logique partielle.
Un nouvel essai qui revalide l'asset et réussit le `fsync` promeut explicitement
sa durabilité vers `DURABLE` dans la transaction DB.

Le sous-schéma introduit en v5 sépare `feature_assets`, `feature_sets` et
`feature_extract_tasks`. L'unicité logique porte sur image, kind, version et
fingerprint. Une tâche est persistée avant enqueue, passe par la queue et le
Governor (CPU, IO, 576 Mio conservateurs, lot 1), puis checkpointée initialement
et terminalement. La reprise recommence l'image entière : il n'existe pas de
fausse reprise intra-ORB. La publication est idempotente.

Pause et annulation sont coopératives avant/après l'appel ORB ; cet appel n'est
pas interruptible. L'image gérée est rehashée avant extraction. Une image
uniforme produit légitimement un Feature Set READY vide.

Lardon3D ne garantit pas des octets ORB identiques entre versions d'OpenCV,
plateformes ou backends. L'idempotence porte sur l'environnement courant et le
contrat `extractor_version`; une évolution qui change durablement la sémantique
ou les octets exige d'auditer et, si nécessaire, d'incrémenter cette version.

## Statut

**IMPLEMENTED** — ORB réel, format v1, assets content-addressed, DB v6,
publication atomique, reader borné, task kind production et reprise automatique.

**NOT_YET_WIRED** — commande de réconciliation/scrub des orphelins, contrôle fin
du backend parallèle OpenCV, orientation EXIF, lancement automatique de
l'extraction après import et planification multi-image/DAG.

**IMPLEMENTED** — Visual Index v1 consomme ce reader sans changer le format.

**PLANNED** — paires candidates, matching, vérification géométrique, tracks et SfM.

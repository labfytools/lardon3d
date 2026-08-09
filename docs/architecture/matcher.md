# Matcher v1 et Match Store v1

## Contrat

Le pipeline v1 est `ORB → BFMatcher Hamming` ou `SIFT/RootSIFT → BFMatcher L2`,
puis KNN `k=2`, Lowe ratio, tri canonique, Match File content-addressed et Match
Result. Il s'arrête avant toute vérification géométrique.

Pour chaque descripteur A, OpenCV fournit zéro, un ou deux voisins. Zéro ne
produit rien. Un seul voisin valide est accepté. Avec deux voisins, le premier
est accepté exactement si `d1 < threshold * d2`; une seconde distance nulle,
une égalité, NaN, Inf ou une distance négative est rejetée. `-0.0` est accepté
comme zéro conformément à IEEE-754. Indices invalides et
distances du premier voisin non finies ou négatives sont rejetés.

Un `feature_index_a` produit donc au maximum une correspondance acceptée.
Sans cross-check, plusieurs indices A peuvent viser le même index B. L'ordre
final est `(feature_index_a, distance, feature_index_b)` croissant. BFMatcher
fournit une liste KNN par query et le filtre ne conserve que son premier voisin :
la déduplication est donc mathématiquement inutile. Le Matcher rejette toute
sortie anormale contenant deux fois le même index A.

## Match File v1

Header fixe de 32 octets, tous les entiers en little-endian :

```
0..3    octets ASCII exacts "L3DM"
4       format_version = 1
5       descriptor_type (1=U8, 2=F32)
6..7    reserved = 0
8..11   match_count uint32
12..15  descriptor_dimension uint32 (32 ou 128)
16..23  feature_set_id_a uint64
24..31  feature_set_id_b uint64
```

Chaque entrée fait 12 octets : index A uint32, index B uint32, distance float32.
Le format accepte zéro entrée pour tester reader/writer, mais le Matcher ne
publie pas d'asset vide. La borne est 8192 entrées et la taille maximale exacte
est `32 + 8192 * 12 = 98336` octets, environ 96 Kio.

Le reader valide avant toute allocation : magic physique, version, reserved,
type/dimension, borne du compte, calcul de taille, taille physique exacte,
Feature Set IDs A/B sans permutation, indices contre les comptes attendus et
distance finie non négative. Fichiers tronqués, trailing bytes et versions
futures sont rejetés.

## Publication et reuse

Pour `NO_MATCH`, le fichier temporaire est supprimé et seule une ligne sans
asset est créée. Pour `MATCHED` : temp dans `assets/matches`, écriture complète,
`fsync` du fichier, SHA-256, chemin `assets/matches/<préfixe>/<sha256>`, puis
publication atomique par `link`. Une race de même contenu est validée; un
contenu différent déjà présent n'est jamais écrasé lors d'une publication
fraîche. Lorsqu'une ligne existante prouve que son asset content-addressed est
corrompu, le Matcher recalcule puis remplace atomiquement cet asset par `rename`
et répare les métadonnées de la même ligne sous transaction. Le temp est nettoyé
et le répertoire final synchronisé.

Le reuse `MATCHED` exige métadonnées complètes, fichier régulier non symbolique,
taille, SHA-256, header, type/dimension, IDs A/B, compte et entrées valides. Il
n'existe aucun fallback faisant confiance au chemin ou à l'existence seuls.
La validation de reuse lit le fichier borné une seule fois et calcule le SHA-256
sur ce même buffer.

## Bornes mémoire et performance

Les deux buffers de descripteurs contigus occupent au maximum 512 Kio pour ORB
ou 8 Mio pour SIFT/RootSIFT. Les résultats KNN ne contiennent que deux DMatch
par query, les matches filtrés et le Match File sont chacun bornés à environ
96 Kio. Le working set contrôlé du Matcher est donc inférieur à environ 10 Mio,
hors scratch interne borné par OpenCV; aucune structure `A × B` n'est
matérialisée.

Le Match File complet est sérialisé dans un buffer heap borné à 98336 octets et
écrit par un unique `write_exact`, puis synchronisé une fois. Les mesures locales
restent dans `.opencode/work/current_ticket.md`, pas dans ce contrat canonique.
À 8192 features, le coût dominant mesuré reste l'évaluation exacte des
distances dans `cv::BFMatcher::knnMatch`; v1 ne remplace pas OpenCV ni BFMatcher.

Le Matcher n'est pas encore un task kind autonome. Lorsqu'il est orchestré par
une tâche, celle-ci doit utiliser l'unique Resource Governor existant avec une
estimation couvrant ce working set; aucune seconde logique de budget n'est
introduite ici.

## Déterminisme et fingerprint

Le fingerprint couvre kind, version, `k=2`, threshold float32 et absence de
cross-check. À Feature Files, configuration, implémentation/version OpenCV et
environnement numérique compatibles identiques, SIFT/RootSIFT garantissent les
mêmes paires attendues, l'ordre et la sérialisation stables. Aucune promesse
bit-à-bit cross-platform n'est faite pour les distances L2 flottantes.
ORB/Hamming bénéficie d'une garantie plus forte grâce à sa distance entière.

NEXT: GEOMETRIC VERIFICATION MODEL

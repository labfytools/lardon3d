# Visual Index v1

## Problème et frontière

Le Visual Index transforme une collection homogène de `FeatureSet` READY en
candidats de recherche. Il consomme exclusivement `feature_set_id` et les
descripteurs ORB lus par le Feature Reader. Il ne fait ni matching final, ni
ratio test, ni vérification géométrique.

## Choix algorithmique

La v1 utilise un LSH binaire déterministe à six tables. Chaque table extrait
24 positions distinctes des 256 bits ORB. La position v1 est
`(41*table + 11*bit) mod 256`; 11 étant premier avec 256, les 24 positions
d'une table sont distinctes. Une clé est `(table_id, key24)`. Des descripteurs proches en
Hamming ont une probabilité élevée de collision dans au moins une table, sans
conversion flottante.

Alternatives évaluées :

- le hash exact est très compact et déterministe, mais son rappel s'effondre
  dès qu'un descriptor varie d'un bit ;
- le multi-index hashing avec sous-chaînes et multiprobes offre des garanties
  Hamming intéressantes, mais le nombre de postings/probes nécessaire au
  rappel utile d'ORB est trop élevé pour une v1 bornée ;
- FLANN-LSH masque son format, ses allocations et sa stabilité de
  sérialisation, ce qui nuit à la reprise et à l'audit ;
- BoW/IVF donne un bon retrieval image, mais impose vocabulaire, entraînement,
  identité et politique de mise à jour avant de pouvoir être incrémental ;
- HNSW et FAISS ajoutent une dépendance et un état mutable complexes sans
  avantage décisif à quelques milliers d'images.

Ce LSH n'est pas un matcher. Il privilégie une base déterministe, segmentable
et contrôlable. Une évolution de la sélection de bits exige une nouvelle
`visual_index_version`.

## Identité et configuration

Le kind est `orb-lsh`, version 1. Un index contient exclusivement des Feature
Sets de même `descriptor_type`, dimension, `extractor_kind`, version et
`parameter_fingerprint`. Sa configuration v1 contient :

- `table_count=6` ;
- `key_bits=24` ;
- `max_features_per_set` entre 1 et 1024, défaut 512 ;
- `max_bucket_postings` entre 1 et 4096, défaut 256 ;
- `max_segments=256` ;
- `max_feature_sets_per_segment=16`.

Le fingerprint de paramètres est SHA-256 des 32 octets canoniques
`L3DVICF1`, version et cinq entiers little-endian. Aucun padding, JSON, locale
ou endianness hôte n'intervient. `visual_index_id` est une identité SQLite
`AUTOINCREMENT`, jamais réutilisée après publication.

## Échantillonnage

Au plus `max_features_per_set` features sont indexées. La sélection v1 retient
le préfixe de `feature_index` croissant. Les postings conservent l'indice
original. Un Feature Set vide est
membre valide sans posting. Le build ne garde qu'un Feature Set et une tranche
Feature Reader de 256 descripteurs en mémoire.

## Segments introduits en Project Database v6, conservés en v7

Un index logique possède des segments immuables READY. Chaque update publie un
segment de un à seize nouveaux Feature Sets, puis ajoute atomiquement segments
et memberships. `UNIQUE(visual_index_id,feature_set_id)` assure l'idempotence.
Une recherche copie au début la liste bornée des segments READY, relâche le
mutex DB, puis lit ce snapshot. Un segment commité au milieu sera visible à la
requête suivante.

SQLite conserve les tables `visual_indexes`, `visual_index_segments`,
`visual_index_memberships` et `visual_index_update_tasks`. Les gros postings
restent hors DB. La configuration et chaque membership sont immuables. La
compaction est `NOT_YET_WIRED`; au-delà de 256 segments une update est refusée avec
`LARDON3D_VISUAL_INDEX_LIMIT`. Avec seize membres par segment, la capacité v1 est donc
exactement 4096 Feature Sets par index. Le refus ne publie ni segment ni membership et
l'index existant reste requêtable.

Le DDL v6 exact est `schema_visual_v6` dans `src/project_db.c`. Il impose
`AUTOINCREMENT` aux index/segments, les uniques
`(visual_index_id,generation)`, `(visual_index_id,sha256)` et
`(visual_index_id,feature_set_id)`, ainsi que les FKs vers index, Feature Set,
segment et tâche. Les CHECKS bornent tables 1..32, bits 8..32, sampling
1..1024, bucket 1..4096, membres segment 1..16 et durabilité 0..1. La migration
entière reste sous `BEGIN IMMEDIATE` et possède une injection de rollback v6.

## Segment File v1

Le fichier est little-endian et ne sérialise aucune structure C. Layout :

| Offset | Taille | Champ |
|---:|---:|---|
| 0 | 8 | magic `L3DVIDX\0` |
| 8 | 4 | format version 1 |
| 12 | 4 | header size 128 |
| 16 | 4 | table count |
| 20 | 4 | key bits |
| 24 | 8 | posting count |
| 32 | 8 | member count |
| 40 | 8 | postings offset, 128 |
| 48 | 8 | total size |
| 56 | 32 | index parameter fingerprint |
| 88 | 32 | feature parameter fingerprint |
| 120 | 8 | réservés, zéro |

Chaque posting fait 24 octets : `table_id:u32`, `key24:u32`,
`feature_set_id:u64`, `feature_index:u32`, réservé zéro `u32`. L'ordre est
`table_id`, clé, Feature Set, feature index. Le fichier exact est SHA-256 et
vit sous `assets/visual-index/<2 hex>/<sha256 lowercase>`.

Publication : temporaire local, écriture, `fsync`, hash, `link` sans
écrasement, validation d'une adoption concurrente, `fsync` du répertoire, puis
transaction DB. Un échec après publication peut laisser un orphelin mais jamais
un segment READY partiel. La durabilité distingue `DURABLE` et
`PUBLISHED_NOT_DURABLE`.

Le reader vérifie le SHA avant le parsing. Un fichier au SHA et aux métadonnées cohérents
mais portant une version future produit `UNSUPPORTED_VERSION`; les comptes et produits
d'offset invalides produisent `CORRUPT` avant allocation, conversion ou lecture de posting.

## Recherche, score et bornes

L'API est centrée sur `(visual_index_id, query_feature_set_id)`. Elle accepte
`ANY_SCANSET`, `SAME_SCANSET` ou `OTHER_SCANSETS`, l'exclusion du même asset,
un minimum de preuves et `top_k` entre 1 et 256. Elle ne retourne jamais le
Feature Set ni l'image de requête.

Une preuve est un `feature_index` de requête distinct ayant au moins une
collision avec le candidat. Plusieurs tables, postings ou descriptors du
candidat ne multiplient pas cette preuve. Le score final vaut
`evidence_count / sampled_query_feature_count` dans `[0,1]`. Le volume du
candidat ne peut donc pas augmenter le score sans preuve distincte. L'ordre est
score décroissant, preuves décroissantes, `image_id`, puis `feature_set_id`.

La burstiness est bornée par une contribution maximum par feature de requête
et candidat. Une première passe additionne la fréquence d'un bucket sur tous
les segments du snapshot. Au-delà de `max_bucket_postings`, il est ignoré : un motif
très commun ne peut ni allouer une liste géante ni dominer le score. Le reader
lit au plus 256 postings par appel. L'accumulateur contient au plus 4096
candidats ; les nouveaux candidats sont ignorés après saturation, de manière
déterministe par l'ordre des postings. Aucun cache global n'existe et un seul
segment est ouvert à la fois.

Deux updates concurrentes peuvent sélectionner le même lot et construire le même asset.
La transaction SQLite et les contraintes uniques ne laissent publier qu'un segment et
un membership par Feature Set; l'autre update échoue/rejoue en no-op. Une query prend son
snapshot de métadonnées avant les lectures et ouvre/ferme un seul segment à la fois, y
compris avec 250 à 256 segments : le nombre de descripteurs de fichier reste borné.

## Tâche, reprise et ressources

`visual_index.update`, version 1, persiste `visual_index_id` et un curseur
`after_feature_set_id`. Une séquence traite au plus seize Feature Sets non
indexés, publie et commit un segment, puis checkpoint. Pause et annulation sont
coopératives entre lectures et avant publication ; un segment déjà READY reste
valide. La reprise recommence au dernier curseur commité et l'unicité des
memberships rend le rejeu idempotent.

L'estimation réserve un thread CPU, un slot I/O, GPU zéro, 8 Mio fixes et 2 Mio
par Feature Set, lot 1..16. `record_batch` reçoit le nombre de Feature Sets
réellement commités, la durée réelle et `peak_memory_bytes=0` (inconnue).

## Complexité et limites

Pour `D` descriptors échantillonnés, construction et disque sont `O(6D)`.
Une requête effectue `O(6Q log P + H)` par segment (`Q<=1024`, `H` hits bornés),
pas `O(images²)`. La mémoire build est bornée par huit métadonnées, 256
descripteurs et les postings d'un segment; la mémoire query par 4096 candidats,
256 postings et 256 résultats. À 3700 images et 512 features, environ 11,4
millions de postings sont produits. Un test structurel persiste 50 000 Feature Sets puis
confirme la pagination par 16 et le refus propre après 4096 memberships. Un index unique
ne couvre donc pas encore 50 000 images : le risque principal est le nombre de segments
et les seeks. Une compaction/base+delta ou une évolution v2 sera nécessaire, sans changer
les identités durables; elle est `NOT_YET_WIRED`.

Les fixtures de validation incluent un Feature Set vide, un crop réel, une rotation de
8 degrés, deux campagnes, un asset source partagé et une attaque de motif répétitif. Ces
tests valident le classement de candidats LSH, jamais une compatibilité géométrique.

## Frontière future

Le Candidate Pair Generator pourra filtrer sur score et `evidence_count`, puis
transmettre `feature_set_id + feature_index` au futur matcher. Le score Visual
Index ne constitue jamais une preuve géométrique.

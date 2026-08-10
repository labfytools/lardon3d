# Base de données projet Lardon3D

> Version courante : **v16**. La migration transactionnelle v15→v16 ajoute
> le modèle persistant Sparse SfM (calibrations, scopes et reconstructions).
> La migration transactionnelle v14→v15 ajoute
> `track_builder_tasks` pour le payload durable explicite du Task Builder. La
> migration transactionnelle v13→v14 ajoute
> les tables `track_sets`, `tracks` et `track_observations` pour le Track
> Model v1. La migration v12→v13 ajoute uniquement `geometric_verifier_tasks`
> pour la tâche durable. La migration v11→v12 ajoute le modèle immutable
> `geometric_verification_results`. La migration v10→v11 ajoute
> `matcher_tasks` pour la tâche Matcher durable. La version v10 publiée ajoute
> uniquement `match_results` pour le Match Result Model. La migration v8→v9
> ajoute la table `candidate_pair_generate_tasks` pour la tâche durable
> Candidate Pair. La migration v7→v8 ajoute la table `candidate_pairs` pour
> le sous-système Candidate Pair.
> Les migrations historiques restent ordonnées et les faults d'injection
> vérifient le rollback.

## Vision

La base de données projet stocke les métadonnées de reconstruction et les relations entre les
entités. Elle est conçue pour être légère, persistante et permettre la reprise après interruption.

## Sparse SfM Gate B (v16)

Le modèle Sparse SfM v1 est publié atomiquement dans les tables
`sparse_calibrations`, `sparse_calibration_scopes`,
`sparse_calibration_scope_images`, `sparse_reconstructions`,
`sparse_reconstruction_components`, `sparse_registered_images`,
`sparse_landmarks` et `sparse_landmark_observations`. Les résultats sont
immuables et les collections volumineuses sont lues par curseurs bornés ; la
base ne charge jamais une reconstruction complète par défaut. Les coordonnées
restent dans le repère arbitraire de chaque composant et les références
d'observation ne dupliquent ni descripteurs ni coordonnées de pixels.
Les relations de suppression disposent d'index enfants dédiés, notamment sur
`calibration_id` dans les membres de scope et `calibration_scope_id` dans les
reconstructions, afin que les vérifications FK restent indexées.

## Structure conceptuelle

### Entités principales

#### Project
- Identifiant unique
- Nom et description
- Date de création
- Configuration
- Chemins des répertoires

#### Scan Set
- Identifiant unique
- Nom de l'acquisition
- Date
- Provenance
- État de traitement

#### Image
- Identifiant unique
- Chemin du fichier
- Métadonnées EXIF
- État de traitement
- Appartenance aux scan sets

#### Feature Set
- Identifiant unique
- Type de descripteur
- Paramètres
- Chemin des données

#### Visual Signature
- Identifiant unique
- Type d'index
- Paramètres
- Chemin des données

#### Candidate Pair
- Identifiant unique (`candidate_pair_id`)
- Image source (`image_id_a`)
- Image cible (`image_id_b`)
- Ordre canonique : `image_id_a < image_id_b`
- Self-pairs interdits
- Unicité persistante
- Date de création (`created_at`)

#### Verified Pair
- Identifiant unique
- Candidate pair source
- Statut (validée, rejetée)
- Métriques

#### Track Set (v14)
- Identifiant unique (`track_set_id`)
- Identité de reuse (builder, verifier, scope)
- Nombre de tracks et GVR
- Immutable après publication

#### Track (v14)
- Identifiant unique (`track_id`)
- Track Set parent
- Nombre d'observations (≥ 2)
- Pas de coordonnées 3D en v1

#### Track Observation (v14)
- Identifiant composite `(track_set_id, feature_set_id, feature_index)`
- Track parent
- Position dans le track (`position_in_track`)
- Feature Set et index de feature

#### Camera
- Identifiant unique
- Modèle
- Paramètres intrinsèques
- Distorsion

#### Pose
- Identifiant unique
- Camera
- Translation
- Rotation
- Qualité

#### Point3D
- Identifiant unique
- Position
- Couleur
- Qualité
- Observations

#### Reconstruction Layer
- Identifiant unique
- Type (sparse, dense, mesh, etc.)
- Provenance
- Transformations
- Qualité
- Chemin des données

#### Measurement
- Identifiant unique
- Type
- Valeur
- Incertitude
- Cible géométrique

#### Document Source
- Identifiant unique
- Type (plan, croquis, etc.)
- Chemin
- Métadonnées

#### Geometric Constraint
- Identifiant unique
- Type
- Paramètres
- Sources
- Poids

#### Artifact
- Identifiant unique
- Type
- État (temporaire, publié)
- Chemin
- Métadonnées

#### Checkpoint
- Identifiant unique
- État du pipeline
- Métadonnées
- Date

## Relations

- Project → Scan Set (1:N)
- Scan Set → Image logique (1:N)
- Image logique → Image Asset (N:1)
- Image → Feature Set (1:N)
- Image → Visual Signature (1:N)
- Candidate Pair → Image (2)
- Verified Pair → Candidate Pair (1)
- Track Set → Track (1:N, CASCADE)
- Track → Track Observation (1:N, CASCADE)
- Track Observation → Feature Set (N:1)
- Camera → Pose (1:N)
- Pose → Reconstruction Layer (N:M)
- Reconstruction Layer → Artifact (1:N)
- Measurement → Point3D (N:1)
- Document Source → Geometric Constraint (N:M)
- Geometric Constraint → Point3D (N:M)
- Artifact → Checkpoint (N:1)

## Invariants

- Chaque entité a un identifiant unique stable
- Les relations sont explicitement définies
- Les artefacts partiels ne sont jamais considérés comme valides
- La reprise commence à la dernière frontière connue

## Frontière avec les checkpoints de tâche

Le modèle durable v1 et son codec fichier sont implémentés indépendamment du
stockage. La base réutilise les mêmes règles de normalisation et de
validation ; elle ne stockera jamais les objets pthread, callbacks, pointeurs,
contrats ou réservations. Le fichier par tâche est une fondation, pas une
Project Database miniature.

La stratégie v1 retient un résumé logique interrogable dans SQLite et une
référence vers le fichier checkpoint. Le fichier checkpoint validé reste la
source complète pour `lardon3d_task_restore()` ; la DB seule ne reconstruit
jamais une tâche. Un écart ou un fichier invalide interdit la reprise.

## Schéma v7 implémenté

- `metadata(key PRIMARY KEY, value)` contient `schema_version=7` et
  `next_task_id`, prochain ID durable allouable.
- `project(singleton=1, stable_id UNIQUE, name, created_at, updated_at)` décrit
  l'unique identité logique de la DB.
- `tasks(task_id PRIMARY KEY, name, task_kind, task_kind_version, saved_state, recovery_state, progress,
  sequence_count, started_sec/nsec, finished_sec/nsec, updated_at)` contient le
  résumé durable. Les IDs v1 sont compris entre 1 et `INT64_MAX`.
- `checkpoints(task_id PRIMARY KEY REFERENCES tasks ON DELETE CASCADE, path,
  format_version, durability, updated_at)` représente `DURABLE` ou
  `PUBLISHED_NOT_DURABLE`.
- `artifacts(artifact_id PRIMARY KEY, kind, path, state, size_bytes,
  producer_task_id REFERENCES tasks, created_at, updated_at)` inventorie des
  fichiers externes. Les états v1 sont `STAGED` et `READY`.
- `scansets(scanset_id INTEGER PRIMARY KEY AUTOINCREMENT, name, created_at, updated_at)`
  représente les acquisitions logiques, y compris les ScanSets vides.
- `image_assets(asset_id INTEGER PRIMARY KEY AUTOINCREMENT, sha256 UNIQUE, path UNIQUE,
  size_bytes, state, created_at)` décrit les contenus physiques `READY`.
- `images(image_id INTEGER PRIMARY KEY AUTOINCREMENT, scanset_id REFERENCES scansets,
  asset_id REFERENCES image_assets, original_name, source_path,
  producer_task_id REFERENCES tasks, imported_at)` décrit les images logiques.
  `UNIQUE(scanset_id,asset_id)` interdit les doublons de contenu dans une même
  acquisition sans fusionner deux acquisitions différentes.
- `image_import_tasks(task_id PRIMARY KEY REFERENCES tasks ON DELETE CASCADE,
  source_path, scanset_id REFERENCES scansets)` conserve les paramètres
  métier immuables de `import.images`.
- `feature_assets` conserve SHA-256, chemin content-addressed, taille,
  durabilité et date de publication des Feature Files hors SQLite.
- `feature_sets` relie image, extracteur/version/fingerprint, hash source,
  type/dimension descriptor, compte, producteur et métriques légères
  `occupied_cells`, `total_cells`, `coverage_ratio` et densité/Mpx.
- `feature_extract_tasks` conserve les paramètres immuables ORB historiques.
- `sift_extract_tasks` conserve kind `sift`/`rootsift`, version 1, limites,
  paramètres OpenCV binary64, grille et fingerprint exacts.
- `feature_support_sets` identifie une consolidation immutable, son image, ses
  deux Feature Sets sources, son rayon et son fingerprint.
- `feature_support_groups` conserve position représentative, distance locale
  et `support_count`; `feature_support_members` normalise chaque référence
  `feature_set_id + feature_index`, sans descriptor SQLite.
- `visual_indexes` conserve l'identité `AUTOINCREMENT`, la configuration
  Feature homogène, les paramètres LSH et leurs fingerprints.
- `visual_index_segments` conserve identité `AUTOINCREMENT`, génération,
  SHA-256, chemin, taille, compteurs, durabilité et tâche productrice.
- `visual_index_memberships` a pour clé primaire
  `(visual_index_id,feature_set_id)` et référence le segment immutable.
- `visual_index_update_tasks` conserve `visual_index_id` et le curseur durable.

Les indexes v6 sont
`visual_index_segments(visual_index_id,generation)` et
`visual_index_memberships(visual_index_segment_id,feature_set_id)`. Les FKs
ciblent `visual_indexes`, `feature_sets`, `visual_index_segments` et `tasks`.
Les postings ne sont jamais stockés dans SQLite.

`AUTOINCREMENT` couvre les identités publiées catalogue, Feature Store et
Visual Index. Il
empêche la réutilisation d'un ID issu d'une transaction validée même si sa ligne
maximale est supprimée plus tard. Le coût de `sqlite_sequence` est accepté pour
garantir qu'un futur Feature Store, match ou track ne voie jamais son identifiant
désigner un autre objet. Les IDs de transactions rollbackées ne sont pas
considérés publiés et peuvent être réutilisés.

Les indexes ajoutés en v4 sont `images(scanset_id,image_id)`, pagination réelle,
et `images(producer_task_id,image_id)`, recherche par tâche productrice. Le
SHA-256 et le chemin asset sont déjà indexés par leurs contraintes `UNIQUE`.

## Schéma v8 implémenté

La migration v7→v8 ajoute la table `candidate_pairs` :

```sql
CREATE TABLE candidate_pairs(
    candidate_pair_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(candidate_pair_id>0),
    image_id_a INTEGER NOT NULL REFERENCES images(image_id),
    image_id_b INTEGER NOT NULL REFERENCES images(image_id),
    created_at INTEGER NOT NULL CHECK(created_at>=0),
    CHECK(image_id_a < image_id_b),
    UNIQUE(image_id_a, image_id_b)
);
CREATE INDEX candidate_pairs_image_a_idx ON candidate_pairs(image_id_a);
CREATE INDEX candidate_pairs_image_b_idx ON candidate_pairs(image_id_b);
```

**Invariants** :
- `image_id_a < image_id_b` : ordre canonique garanti par CHECK SQL
- Self-pairs interdits (impliqué par `image_id_a < image_id_b`)
- `UNIQUE(image_id_a, image_id_b)` : unicité persistante
- `created_at` : timestamp Unix secondes, non-négatif

**API** :
- `lardon3d_project_db_create_candidate_pair()` — INSERT avec canonicalisation
- `lardon3d_project_db_load_candidate_pair()` — SELECT par ID
- `lardon3d_project_db_find_candidate_pair()` — SELECT par (image_a, image_b)
- `lardon3d_project_db_list_candidate_pairs()` — SELECT paginé ORDER BY id

**Notes** :
- Le score et la source ne sont pas persistés dans cette version
- La génération est déterministe pour mêmes entrées/configuration
- L'idempotence est garantie par find avant create

## Schéma v9 implémenté

La migration v8→v9 ajoute la table `candidate_pair_generate_tasks` :

```sql
CREATE TABLE candidate_pair_generate_tasks(
    task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id) ON DELETE CASCADE,
    visual_index_id INTEGER NOT NULL CHECK(visual_index_id>0),
    after_feature_set_id INTEGER NOT NULL CHECK(after_feature_set_id>=0),
    top_k INTEGER NOT NULL CHECK(top_k>0 AND top_k<=256),
    minimum_evidence_count INTEGER NOT NULL CHECK(minimum_evidence_count>=0
        AND minimum_evidence_count<=1024),
    scanset_filter INTEGER NOT NULL CHECK(scanset_filter>=0 AND scanset_filter<=2),
    exclude_same_asset INTEGER NOT NULL CHECK(exclude_same_asset IN (0,1))
);
```

**Invariants** :
- `task_id` référence `tasks(task_id)` avec ON DELETE CASCADE
- `after_feature_set_id` : curseur de reprise, non-négatif
- `top_k` : borné entre 1 et 256 (LARDON3D_VISUAL_INDEX_TOP_K_MAX)
- `minimum_evidence_count` : borné entre 0 et 1024
- `scanset_filter` : 0=ANY, 1=CURRENT, 2=OTHER

**API** :
- `lardon3d_project_db_record_candidate_pair_generate_task()` — UPSERT checkpoint
- `lardon3d_project_db_load_candidate_pair_generate_task()` — SELECT par task_id

## Schéma v10 publié

La migration v9→v10 ajoute uniquement `match_results` pour le Match Result
Model. Son schéma et ses invariants restent inchangés.

## Schéma v11 implémenté

La migration v10→v11 ajoute `matcher_tasks`. Cette table conserve uniquement
la configuration immutable et le curseur durable :

```sql
CREATE TABLE matcher_tasks(
    task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id) ON DELETE CASCADE,
    after_candidate_pair_id INTEGER NOT NULL
        CHECK(after_candidate_pair_id>=0),
    feature_extractor_kind TEXT NOT NULL,
    feature_extractor_version INTEGER NOT NULL
        CHECK(feature_extractor_version>0),
    feature_parameter_fingerprint BLOB NOT NULL
        CHECK(length(feature_parameter_fingerprint)=32),
    matcher_kind INTEGER NOT NULL CHECK(matcher_kind BETWEEN 0 AND 2),
    ratio_threshold REAL NOT NULL
        CHECK(ratio_threshold>0.0 AND ratio_threshold<1.0)
);
```

Le curseur est le dernier `candidate_pair_id` checkpointé. Il n'implique ni
continuité des IDs ni liste persistée de Candidate Pairs. La configuration ne
peut pas changer lors d'un UPSERT ; seul le curseur avance.

## Schéma v13 implémenté

La migration v12→v13 ajoute uniquement `geometric_verifier_tasks`. Elle porte
le curseur `after_match_result_id`, les sept paramètres scientifiques v1 et le
fingerprint de contrôle. L'UPSERT autorise seulement le curseur à évoluer ; la
configuration reste immuable. La migration est transactionnelle, son rollback
forcé conserve une vraie v12 sans la table et un retry termine en v13.

Le Match Result ci-dessous reste le contrat publié de v10 :

```sql
CREATE TABLE match_results(
    match_result_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(match_result_id>0),
    candidate_pair_id INTEGER NOT NULL
        REFERENCES candidate_pairs(candidate_pair_id),
    feature_set_id_a INTEGER NOT NULL
        REFERENCES feature_sets(feature_set_id),
    feature_set_id_b INTEGER NOT NULL
        REFERENCES feature_sets(feature_set_id),
    matcher_kind TEXT NOT NULL
        CHECK(length(matcher_kind)>0 AND length(matcher_kind)<=64),
    matcher_version INTEGER NOT NULL CHECK(matcher_version>0),
    parameter_fingerprint BLOB NOT NULL
        CHECK(length(parameter_fingerprint)=32),
    result_status INTEGER NOT NULL CHECK(result_status IN (0,1)),
    match_count INTEGER NOT NULL CHECK(match_count>=0 AND match_count<=8192),
    match_asset_sha256 BLOB CHECK(match_asset_sha256 IS NULL OR
        length(match_asset_sha256)=32),
    match_asset_path TEXT CHECK(match_asset_path IS NULL OR
        length(match_asset_path)>0),
    match_asset_size_bytes INTEGER CHECK(match_asset_size_bytes IS NULL OR
        match_asset_size_bytes>0),
    created_at INTEGER NOT NULL CHECK(created_at>=0),
    CHECK((result_status=0 AND match_count=0 AND match_asset_sha256 IS NULL
           AND match_asset_path IS NULL AND match_asset_size_bytes IS NULL)
       OR (result_status=1 AND match_count>0 AND match_asset_sha256 IS NOT NULL
           AND match_asset_path IS NOT NULL AND match_asset_size_bytes IS NOT NULL)),
    UNIQUE(candidate_pair_id, feature_set_id_a, feature_set_id_b,
           matcher_kind, matcher_version, parameter_fingerprint)
);
CREATE INDEX match_results_candidate_pair_idx
    ON match_results(candidate_pair_id);
CREATE INDEX match_results_feature_set_a_idx
    ON match_results(feature_set_id_a);
CREATE INDEX match_results_feature_set_b_idx
    ON match_results(feature_set_id_b);
```

**Invariants** :
- `UNIQUE(candidate_pair_id, feature_set_id_a, feature_set_id_b, matcher_kind, matcher_version,
  parameter_fingerprint)` : identité déterministe 6 parties
- `candidate_pair_id` référence `candidate_pairs(candidate_pair_id)` avec l'action par défaut
  (NO ACTION)
- `feature_set_id_a` et `feature_set_id_b` référencent `feature_sets(feature_set_id)`
- `feature_set_id_a` appartient à `image_id_a` de la Candidate Pair, `feature_set_id_b` appartient
  à `image_id_b` (validé par l'API create)
- `NO_MATCH` impose `match_count=0` et aucun asset
- `MATCHED` impose `match_count>0` et SHA/path/taille complets
- les échecs d'exécution restent dans le Task Runtime et ne créent pas de ligne
- `matcher_kind` borné à 64 caractères
- `parameter_fingerprint` exactement 32 octets (SHA-256)

**API** :
- `lardon3d_project_db_create_match_result()` — INSERT avec validation des contraintes
- `lardon3d_project_db_load_match_result()` — SELECT par ID
- `lardon3d_project_db_find_match_result()` — SELECT par (candidate_pair_id, feature_set_id_a,
  feature_set_id_b, matcher_kind, matcher_version, parameter_fingerprint)
- `lardon3d_project_db_list_match_results()` — SELECT paginé ORDER BY id
- `lardon3d_project_db_record_matcher_task()` — UPSERT configuration/curseur
- `lardon3d_project_db_load_matcher_task()` — SELECT par task_id

## Schéma v12 implémenté

La migration v11→v12 ajoute uniquement `geometric_verification_results`. Le
parent est un Match Result `MATCHED`; sa validation interligne reste dans l'API.

Schéma abrégé (la chaîne SQL exécutable canonique reste dans `src/project_db.c`) :

```sql
CREATE TABLE geometric_verification_results(
    geometric_verification_result_id INTEGER PRIMARY KEY AUTOINCREMENT
        CHECK(geometric_verification_result_id>0),
    match_result_id INTEGER NOT NULL
        REFERENCES match_results(match_result_id) ON DELETE CASCADE,
    verifier_kind INTEGER NOT NULL CHECK(verifier_kind=1),
    verifier_version INTEGER NOT NULL
        CHECK(verifier_version>0 AND verifier_version<=4294967295),
    parameter_fingerprint BLOB NOT NULL
        CHECK(length(parameter_fingerprint)=32),
    status INTEGER NOT NULL CHECK(status IN (1,2)),
    inlier_count INTEGER NOT NULL
        CHECK(inlier_count>=0 AND inlier_count<=8192),
    inlier_mask BLOB NOT NULL
        CHECK(length(inlier_mask)>=1 AND length(inlier_mask)<=1024),
    model_m00 REAL, model_m01 REAL, model_m02 REAL,
    model_m10 REAL, model_m11 REAL, model_m12 REAL,
    model_m20 REAL, model_m21 REAL, model_m22 REAL,
    created_at INTEGER NOT NULL CHECK(created_at>=0),
    CHECK(/* REJECTED: neuf NULL ; VERIFIED: neuf non-NULL */),
    UNIQUE(match_result_id, verifier_kind, verifier_version,
           parameter_fingerprint)
);
CREATE INDEX geometric_verification_results_parent_idx
    ON geometric_verification_results(
        match_result_id, geometric_verification_result_id
    );
```

Les valeurs stables sont FUNDAMENTAL=1, GEOMETRIC_REJECTED=1 et
GEOMETRIC_VERIFIED=2. REJECTED interdit le modèle ; VERIFIED exige neuf valeurs
finies. Pour les deux états, l'API impose taille canonique, padding nul et
popcount exact du masque, ainsi que `inlier_count <= parent.match_count`.
L'index parent sert la liste paginée ; la contrainte UNIQUE sert le find exact.
Le contrat complet, dont l'ordre des bits, est dans
`geometric_verification.md`.

## Schéma v14 implémenté

La migration v13→v14 ajoute les tables `track_sets`, `tracks` et
`track_observations` pour le Track Model v1. Le schéma complet est dans
`tracks.md`. Schéma abrégé (la chaîne SQL exécutable canonique reste dans
`src/project_db.c`) :

```sql
CREATE TABLE track_sets(
    track_set_id INTEGER PRIMARY KEY AUTOINCREMENT
        CHECK(track_set_id > 0),
    builder_kind TEXT NOT NULL
        CHECK(length(builder_kind) > 0 AND length(builder_kind) <= 64),
    builder_version INTEGER NOT NULL CHECK(builder_version > 0),
    parameter_fingerprint BLOB NOT NULL
        CHECK(length(parameter_fingerprint) = 32),
    verifier_kind INTEGER NOT NULL CHECK(verifier_kind > 0),
    verifier_version INTEGER NOT NULL CHECK(verifier_version > 0),
    verifier_fingerprint BLOB NOT NULL
        CHECK(length(verifier_fingerprint) = 32),
    input_scope_hash BLOB NOT NULL
        CHECK(length(input_scope_hash) = 32),
    gvr_count INTEGER NOT NULL CHECK(gvr_count >= 1),
    track_count INTEGER NOT NULL CHECK(track_count >= 0),
    created_at INTEGER NOT NULL CHECK(created_at >= 0),
    UNIQUE(builder_kind, builder_version, parameter_fingerprint,
           verifier_kind, verifier_version, verifier_fingerprint,
           input_scope_hash)
);

CREATE TABLE tracks(
    track_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(track_id > 0),
    track_set_id INTEGER NOT NULL
        REFERENCES track_sets(track_set_id) ON DELETE CASCADE,
    observation_count INTEGER NOT NULL CHECK(observation_count >= 2)
);

CREATE INDEX tracks_set_idx
    ON tracks(track_set_id, track_id);

CREATE TABLE track_observations(
    track_set_id INTEGER NOT NULL,
    track_id INTEGER NOT NULL
        REFERENCES tracks(track_id) ON DELETE CASCADE,
    feature_set_id INTEGER NOT NULL
        REFERENCES feature_sets(feature_set_id),
    feature_index INTEGER NOT NULL CHECK(feature_index >= 0),
    position_in_track INTEGER NOT NULL CHECK(position_in_track >= 0),
    PRIMARY KEY(track_set_id, feature_set_id, feature_index),
    UNIQUE(track_id, position_in_track)
);

CREATE INDEX track_observations_lookup_idx
    ON track_observations(feature_set_id, feature_index, track_set_id);
```

**Invariants SQL** :
- `PRIMARY KEY(track_set_id, feature_set_id, feature_index)` : dans un Track
  Set donné, une observation n'apparaît qu'une fois
- `REFERENCES tracks(track_id) ON DELETE CASCADE` : supprimer un track
  supprime ses observations
- `REFERENCES feature_sets(feature_set_id)` : le Feature Set existe
- `CHECK(observation_count >= 2)` : minimum structurel
- `UNIQUE(builder_kind, builder_version, parameter_fingerprint,
  verifier_kind, verifier_version, verifier_fingerprint, input_scope_hash)` :
  identité de reuse sur `track_sets`
- `ON DELETE CASCADE` depuis `track_sets` : supprimer un set supprime tout
- `UNIQUE(track_id, position_in_track)` : chaque position dans un track est
  unique

**Invariants API** (non protégés par le schéma SQL) :
- `track_set_id` dans `track_observations` correspond au `track_set_id` du
  `track_id` parent
- Une seule observation par image par track (validation via
  `feature_sets.image_id`)
- `feature_index < feature_sets.feature_count`
- `observation_count` cohérent avec le nombre réel d'observations
- `track_count` cohérent avec le nombre réel de tracks

**Statut** : les tables sont créées par la migration et validées par les
tests. L'API C (`create_track_set`, `load_track_set`, `find_track_set`,
`list_track_sets`, `load_track`, `list_tracks`, `find_track_by_observation`)
est exposée et implémentée. Le Track Builder algorithmique et sa tâche durable
sont implémentés par les Gates A–E ; la triangulation reste hors périmètre.

## Schéma v15 — payload durable Track Builder

La migration v14→v15 ajoute uniquement `track_builder_tasks`. Elle ne modifie
aucune table du Track Model et ne change aucune identité scientifique. La ligne
référence un fichier de scope atomiquement publié sous
`.lardon3d/checkpoints/<task_id>.scope` et conserve sa taille, son SHA-256,
son format, le sélecteur exact, le fingerprint Builder, `gvr_count` et
`input_scope_hash`. Le fichier contient `L3DTSCP1`, une version explicite, le
nombre d'IDs et des uint64 little-endian triés et uniques. Le contenu est
rejoué depuis le début après interruption ; aucun curseur ne peut perdre des
arêtes transitoires.

La reconstruction vérifie format, taille, checksum, bornes, tri, unicité,
sélecteur, fingerprint et L3DTSIS1 avant de créer un callback neuf. Une
corruption rend la tâche inexécutable sans créer de Track Set. Les tests
couvrent une vraie base v14, migration, rollback injecté, retry,
fermeture/réouverture et payload corrompu.

## Ouverture et migrations

Une DB vide reçoit la chaîne de schémas jusqu'à v15 dans une transaction
`BEGIN IMMEDIATE`. Une DB v1 reçoit transactionnellement les colonnes nullable
`task_kind` et `task_kind_version`, puis les migrations v2→v3. Les anciennes lignes restent
`NULL/NULL`, sans type inventé et sans perte des projets, tâches, checkpoints ou
artefacts. Une interruption ou erreur provoque un rollback complet. Les DB v1
à v14 sont migrées séquentiellement vers v15.
Une v10 publiée est validée comme telle avant que v10→v11 crée
`matcher_tasks` ; son absence n'est donc pas une corruption. Une version future est refusée et une DB contenant
des tables sans métadonnée de version est considérée corrompue. La fonction
interne de migration applique uniquement la chaîne séquentielle connue jusqu'à
v15 ; une valeur hors de 1..15 est refusée. La failure injectée v12 rollbacke
la table, l'index et le changement de version, laissant une vraie v11 utilisable.
La failure injectée v13 conserve une vraie v12 sans `geometric_verifier_tasks` ;
un retry applique ensuite v12→v13. La failure injectée v14 conserve une vraie
v13 sans `track_sets` ; un retry applique ensuite v13→v14. La failure injectée
v15 conserve une vraie v14 sans `track_builder_tasks` ; un retry applique
ensuite v14→v15.

Migration v1→v2 exacte, exécutée entre `BEGIN IMMEDIATE` et `COMMIT` :

```sql
ALTER TABLE tasks ADD COLUMN task_kind TEXT;
ALTER TABLE tasks ADD COLUMN task_kind_version INTEGER
    CHECK(task_kind_version IS NULL OR task_kind_version > 0);
UPDATE metadata SET value=2
    WHERE key='schema_version' AND value=1;
```

Migration v2→v3 exacte, exécutée entre `BEGIN IMMEDIATE` et `COMMIT` :

```sql
CREATE TABLE image_import_tasks(
    task_id INTEGER PRIMARY KEY REFERENCES tasks(task_id) ON DELETE CASCADE,
    source_path TEXT NOT NULL
);
INSERT INTO metadata(key,value)
VALUES('next_task_id',(
    SELECT CASE
        WHEN COALESCE(MAX(task_id),0)>=9223372036854775807 THEN 0
        ELSE COALESCE(MAX(task_id),0)+1
    END FROM tasks
));
UPDATE metadata SET value=3
    WHERE key='schema_version' AND value=2;
```

Migration v3→v4 exacte, dans la même transaction :

```sql
CREATE TABLE scansets(
    scanset_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(scanset_id>0),
    name TEXT NOT NULL CHECK(length(name)>0 AND length(name)<256),
    created_at INTEGER NOT NULL CHECK(created_at>=0),
    updated_at INTEGER NOT NULL CHECK(updated_at>=created_at)
);
CREATE TABLE image_assets(
    asset_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(asset_id>0),
    sha256 BLOB NOT NULL UNIQUE CHECK(length(sha256)=32),
    path TEXT NOT NULL UNIQUE CHECK(length(path)>0 AND length(path)<4096),
    size_bytes INTEGER NOT NULL CHECK(size_bytes>=0),
    state INTEGER NOT NULL CHECK(state=1),
    created_at INTEGER NOT NULL CHECK(created_at>=0)
);
CREATE TABLE images(
    image_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(image_id>0),
    scanset_id INTEGER NOT NULL REFERENCES scansets(scanset_id),
    asset_id INTEGER NOT NULL REFERENCES image_assets(asset_id),
    original_name TEXT NOT NULL CHECK(length(original_name)>0 AND length(original_name)<256),
    source_path TEXT NOT NULL CHECK(length(source_path)>0 AND length(source_path)<4096),
    producer_task_id INTEGER REFERENCES tasks(task_id),
    imported_at INTEGER NOT NULL CHECK(imported_at>=0),
    UNIQUE(scanset_id,asset_id)
);
CREATE INDEX images_scanset_idx ON images(scanset_id,image_id);
CREATE INDEX images_producer_idx ON images(producer_task_id,image_id);
ALTER TABLE image_import_tasks
    ADD COLUMN scanset_id INTEGER REFERENCES scansets(scanset_id);
INSERT INTO scansets(name,created_at,updated_at)
    SELECT 'Imports antérieurs à ScanSet v1',0,0
    WHERE EXISTS(SELECT 1 FROM image_import_tasks);
UPDATE image_import_tasks
    SET scanset_id=(SELECT scanset_id FROM scansets
                    WHERE name='Imports antérieurs à ScanSet v1'
                    ORDER BY scanset_id LIMIT 1)
    WHERE scanset_id IS NULL;
INSERT INTO metadata(key,value)
    VALUES('legacy_image_catalog_pending',
           CASE WHEN EXISTS(SELECT 1 FROM image_import_tasks) THEN 1 ELSE 0 END);
UPDATE metadata SET value=4
    WHERE key='schema_version' AND value=3;
```

Configuration v7 : `foreign_keys=ON`, `journal_mode=DELETE`,
`synchronous=FULL`, `busy_timeout=5000`. Le mode DELETE convient au propriétaire
unique actuel, évite les fichiers WAL/SHM durables et conserve la synchronisation
forte. Le timeout borne l'attente d'un verrou externe à cinq secondes.

## Concurrence et ownership

Une connexion opaque est sérialisée par un mutex interne. Chaque opération
composée possède sa transaction entière ; aucune transaction publique ne peut
rester ouverte entre deux appels. Aucune I/O d'artefact n'a lieu sous le mutex :
le module confirme que le fichier publié et validé par l'appelant est régulier
avant la mise à jour `READY`. Fermer la DB pendant un
appel concurrent est interdit au propriétaire.

Les records et chaînes sont copiés dans des buffers fournis par l'appelant ;
aucun pointeur SQLite n'en sort. Tous les statements sont finalisés dans
l'appel. La liste de reprise utilise des pages fournies par l'appelant, limitées
à 256 entrées. Elle ne retourne que les tâches normalisées `PENDING` possédant
une référence checkpoint ; le fichier doit encore être chargé et validé.

## Branchement au projet

`Lardon3DAppState` est l'instance projet runtime actuelle et possède exactement
une `Lardon3DProjectDb *` pendant que `project_loaded` est vrai. La DB canonique
est `<project_root>/project.db`. Elle est ouverte à la création/ouverture du
projet et fermée une seule fois par `lardon3d_project_close()`. À l'arrêt de
l'application, la task queue est arrêtée avant la fermeture du projet et de sa
DB. Une fermeture concurrente à un appel projet/DB est interdite au propriétaire.

`project.ini` v2 contient `name`, `stable_id` hexadécimal sur 128 bits et
`version=2`. La même identité est enregistrée dans la table `project`. Toute
divergence est une erreur. Un INI v1 sans identité adopte l'identité d'une DB
existante ; sans DB, une identité est générée une seule fois puis l'INI est
migré atomiquement avant de devenir la référence des ouvertures suivantes. Une
DB existante sans ligne projet ne peut être initialisée que si l'INI possède
déjà son identité.

Les checkpoints sont référencés par chemins relatifs portables :
`.lardon3d/checkpoints/<task_id>.chk`. L'inventaire projet pagine les tâches DB,
résout ce chemin sous la racine, charge le checkpoint et vérifie la cohérence du
snapshot avec le résumé DB.

Après copie du record hors mutex SQLite, l'inventaire consulte la registry. Il
distingue `LEGACY_UNTYPED`, `UNKNOWN_TASK_KIND` et
`UNSUPPORTED_TASK_KIND_VERSION`. Aucun reconstructeur métier n'est appelé sous
le mutex DB. Un upsert ne peut pas changer le couple kind/version d'un task ID.

À l'ouverture, le projet lit des pages de 8 dans l'ordre croissant des task
IDs. Chaque record est copié hors mutex DB avant reconstruction et enqueue. Une
fenêtre pleine interrompt le scan sans modifier les tâches restantes. Le résumé
borné expose `inspected`, `resumed`, `skipped`, `failed`, le nombre de
checkpoints `PUBLISHED_NOT_DURABLE` repris et la saturation éventuelle.

Les erreurs d'ouverture/migration, de schéma ou d'identité restent fatales.
Les erreurs propres à une tâche — legacy, kind inconnu/futur, checkpoint
absent/invalide/futur, source indisponible ou reconstruction — sont non fatales.
Un `BUSY` après le timeout SQLite arrête le scan sans boucle et laisse le projet
ouvert.

## Statut

**IMPLEMENTED** — SQLite système, schéma v15 et migrations séquentielles
v1→v2→v3→v4→v5→v6→v7→v8→v9→v10→v11→v12→v13→v14→v15, identité projet, transactions
tâche+checkpoint, pagination de reprise et artefacts génériques.

**IMPLEMENTED** — ouverture/fermeture avec le projet, identité INI/DB cohérente,
publication de checkpoints par le projet et inventaire de reprise validé.

**IMPLEMENTED** — kinds persistants, classification par registry et
reconstruction explicite testée hors scheduler.

**IMPLEMENTED** — allocation transactionnelle de task IDs, paramètres
immuables de `import.images` et reconstruction production explicite.

**IMPLEMENTED** — reprise automatique sélective, pagination de 8, ordre par ID,
fenêtre de queue non bloquante et résumé consultable.

**IMPLEMENTED** — ScanSets, images logiques, assets SHA-256 et pagination
bornée à 256. Les identités sont des `INTEGER PRIMARY KEY AUTOINCREMENT` SQLite
allouées sous transaction ; aucun `SELECT MAX()+1` n'est utilisé en
fonctionnement normal et une identité validée n'est jamais réutilisée.

La migration v3→v4 ne lit pas `manifest.tsv`. Elle crée le ScanSet legacy et
positionne `metadata.legacy_image_catalog_pending=1` dès qu'une ancienne tâche
d'import existe. Cet indicateur signifie « données legacy potentiellement non
cataloguées », pas « images migrées ».

**IMPLEMENTED** — Track Model v1 : tables `track_sets`, `tracks` et
`track_observations` créées par la migration v13→v14, contraintes SQL
(unicité d'identité de reuse, CASCADE, observation unique par set) et
tests de migration/failure validés. Le schéma complet et les invariants
sont documentés dans `tracks.md`.

**IMPLEMENTED** — Track Builder v1 durable : table `track_builder_tasks`, scope
asset atomique, payload v1 validé, migration v14→v15 et rollback/retry testés.

**IMPLEMENTED** — Sparse SfM v1 persistant : modèle v16 immutable, publication
atomique, composants déterministes, lecteurs bornés, corruption/lifecycle
validation, migration v15→v16 et comparateur fresh/migrated validés par Gate B.
Le modèle de persistance est gelé pour v1 ; le solveur numérique reste hors de
Project DB v16.

**IMPLEMENTED** — API C Track Model v1 : header `project_db.h` et
source `project_db.c` exposent `create_track_set`, `load_track_set`,
`find_track_set`, `list_track_sets`, `load_track`, `list_tracks`,
`find_track_by_observation` et `free_track`. Contraintes et
invariants documentés dans `tracks.md`. Limites réelles : pas de
Track Builder algorithmique, pas de tâche dédiée, pas de
triangulation.

**NOT_YET_WIRED** — autosave à toutes les transitions, retry UI des sources
indisponibles, migration de la TUI legacy et réconciliation des fichiers
orphelins et compaction Visual Index. Feature Store et Visual Index v1 sont implémentés.
Visual Index v1 borne un index à 256 segments de 16 memberships, soit 4096 Feature Sets;
la couverture de 50 000 Feature Sets nécessitera la compaction ou une évolution v2.

**PLANNED** — dépendances d'artefacts, graphe géométrique et
reconstruction incrémentale.

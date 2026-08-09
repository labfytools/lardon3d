# Track Model v1

## Scope

Track Model v1 est le contrat persistant qui transforme les correspondances
géométriquement vérifiées en structures multi-view cohérentes. Il stocke des
ensembles d'observations 2D liées à un même point physique supposé. Il ne
calcule rien, ne triangule pas, ne contient aucune coordonnée 3D et ne résout
aucun conflit. Le Track Builder, la triangulation, le Sparse SfM et le Bundle
Adjustment sont des étapes séparées ; Gate E a gelé le Builder v1 sans
implémenter ces étapes 3D.

## Track definition

Un **Track** est un ensemble d'observations 2D cohérentes d'un même point
physique supposé, observé à travers plusieurs images. Chaque observation est
identifiée par `(feature_set_id, feature_index)`.

Un Track n'est **pas** un point 3D. Il ne contient aucune coordonnée 3D,
aucune erreur de reprojection, aucun statut de triangulation. La
triangulation appartient à une étape ultérieure.

La chaîne scientifique correcte est :

```text
Matcher → Match Result → Geometric Verification → Track Builder v1
→ Track Model → Sparse SfM (futur)
```

Le Matcher ne produit pas les Tracks. Le Track Builder v1 les assemble à partir
des Geometric Verification Results.

## Observation identity

Une observation est identifiée par :

```
(feature_set_id, feature_index)
```

- `feature_set_id` : identifiant SQLite AUTOINCREMENT du Feature Set. Le
  Feature Set porte directement `image_id` comme colonne NOT NULL FK. L'image
  est dérivable par `SELECT image_id FROM feature_sets WHERE feature_set_id=?`.
- `feature_index` : ordinal zero-based dans le tableau de keypoints du Feature
  File, stable tant que le Feature Set existe. Un Feature Set publié est
  immutable : aucune API de production ne modifie ses colonnes après INSERT.

L'identité `(feature_set_id, feature_index)` est suffisante. Il est inutile
de porter `image_id` dans la table d'observations car il est dérivable via
`feature_sets.image_id`.

Note : `feature_sets` ne possède pas de colonne d'état. L'existence d'une
ligne publiée dans la table constitue le contrat réel de disponibilité du
Feature Set.

## Scientific inputs

Les Tracks sont construits exclusivement à partir de :

```
Geometric Verification Result
  status == GEOMETRIC_VERIFIED (2)
```

correspondant exactement au VERIFICATION_SELECTOR du Track Set.

Pour chaque résultat vérifié, les entrées du Match File dont le bit
correspondant dans le masque d'inliers vaut 1 fournissent les correspondances
valides. La chaîne de dérivation est :

```text
GVR → match_result_id
  → candidate_pair + feature_set_id_a + feature_set_id_b
  → Match File entry[i] = (feature_index_a, feature_index_b, distance)
  → bit i du masque d'inliers = 1
  → observation A: (feature_set_id_a, feature_index_a)
  → observation B: (feature_set_id_b, feature_index_b)
```

Un `GEOMETRIC_REJECTED` ne produit aucun track. Un Match Result non vérifié
géométriquement ne suffit pas.

## VERIFICATION_SELECTOR

Le VERIFICATION_SELECTOR définit la configuration de Geometric Verification
éligible pour un Track Set. Il est stocké sur le Track Set et fait partie de
son identité de reuse.

```
(
    verifier_kind      INTEGER,  -- ex: 1 = FUNDAMENTAL
    verifier_version   INTEGER,
    parameter_fingerprint BLOB(32)
)
```

Le Track Builder ne consomme que les GVR avec `status == GEOMETRIC_VERIFIED`
correspondant exactement à ce tuple. Aucune sélection par timestamp, "latest"
ou ordre d'insertion n'est permise.

Valeur production : `(1, 1, SHA-256 de l'encodage canonique 84 octets)`.

## INPUT_SCOPE

L'INPUT_SCOPE représente l'ensemble scientifique réel des entrées consommées
par une Track Generation donnée. Il est distinct du VERIFICATION_SELECTOR :
le selector dit quels GVR sont admissibles, le scope dit quels GVR ont
effectivement été consommés.

```
input_scope_hash  BLOB(32)  -- SHA-256 canonique
gvr_count         INTEGER   -- nombre de GVR consommés
```

### INPUT_SCOPE_HASH

| Propriété | Valeur |
|-----------|--------|
| Domain/version | `L3DTSIS1` (8 octets ASCII) |
| Items | `geometric_verification_result_id` des GVR consommés |
| Canonical ordering | IDs triés par ordre croissant |
| Serialization | Chaque ID : 8 octets little-endian |
| Digest | SHA-256 |
| DB-local IDs | OUI — le reuse est scoped à une DB projet |
| Duplicate handling | Inutile — les IDs sont uniques par construction |
| Empty scope | Interdit — un Track Set sans GVR n'a pas de sens |

Le digest est calculé sur `L3DTSIS1` (8 octets) suivi des IDs sérialisés :
`SHA-256(L3DTSIS1 || id_0 || id_1 || ... || id_N)` où chaque `id_i` est
8 octets little-endian et les IDs sont triés par ordre croissant.

Le `gvr_count` est stocké comme métadonnée de validation. Il permet de
détecter un scope incomplet sans re-hasher. Il ne fait pas partie du hash
lui-même.

Le scope_hash est DB-local : il utilise les `geometric_verification_result_id`
SQLite. Deux DB distinctes avec les mêmes données produiront des IDs
différents. Le reuse est donc scoped à une seule DB projet.

## Track membership invariants

1. **Minimum structurel** : un Track contient au moins 2 observations.
   Une seule observation ne constitue aucune relation multi-view. Le futur
   Track Builder v1, la triangulation ou le Sparse SfM pourront appliquer des
   critères plus stricts. Le Model ne fixe pas de plafond de reconstruction.

2. **One observation per image** : un Track ne contient pas deux observations
   issues de la même image. Cette contrainte est validée par l'API lors de la
   création. Le schéma v1 ne dénormalise pas `image_id` dans
   `track_observations` ; l'API vérifie déterministement la relation via
   `feature_sets.image_id` avant publication sous `BEGIN IMMEDIATE`.

   **SQL** : non protégé (pas de colonne `image_id` dans `track_observations`).
   **API** : validation par jointure `feature_sets.image_id` avant INSERT.

3. **Observation unique across tracks** : dans un même Track Set, une
   observation `(feature_set_id, feature_index)` n'appartient qu'à un seul
   Track.

   **SQL** : `PRIMARY KEY(track_set_id, feature_set_id, feature_index)` sur
   `track_observations`. Le `track_set_id` est dénormalisé depuis `tracks`.
   **API** : validation que `track_set_id` correspond au `track_set_id` du
   `track_id` parent.

4. **Feature Set existence** : chaque `feature_set_id` référencé existe dans
   la table `feature_sets`. La FK SQLite garantit la référence.

   **SQL** : `REFERENCES feature_sets(feature_set_id)`.

5. **Feature index bounds** : `feature_index < feature_sets.feature_count`
   pour l'observation correspondante.

   **SQL** : `CHECK(feature_index >= 0)`.
   **API** : validation de la borne supérieure via `feature_sets.feature_count`
   (SQLite CHECK ne peut pas référencer une autre table).

## Track identity

Un Track persistant possède un identifiant opaque :

```
track_id INTEGER PRIMARY KEY AUTOINCREMENT CHECK(track_id > 0)
```

Il n'a pas d'identité scientifique dérivée de son contenu en v1. Les raisons :

- un hash de membership rendrait les INSERTs dépendants de l'ordre ;
- le contenu d'un track peut être reconstruit depuis les GVR sources ;
- un `track_id` opaque suffit pour la persistance, le référencement et la
  pagination ;
- la corruption est détectée par cohérence interne (doublons, images
  manquantes, index hors bornes) plutôt que par re-hash.

La reproductibilité est assurée au niveau du Track Set (parent), pas du Track
individuel.

## Track Set / Generation

Un **Track Set** est le parent obligatoire de tout Track persistant. Il
représente une génération complète de Track Building.

Champs :

```
track_set_id              INTEGER PK AUTOINCREMENT
builder_kind              TEXT(1..64)
builder_version           INTEGER > 0
parameter_fingerprint     BLOB(32)
verifier_kind             INTEGER   -- VERIFICATION_SELECTOR
verifier_version          INTEGER
verifier_fingerprint      BLOB(32)
input_scope_hash          BLOB(32)
gvr_count                 INTEGER >= 1
track_count               INTEGER >= 0
created_at                INTEGER >= 0
```

### Identité de reuse

```
(
    builder_kind,
    builder_version,
    parameter_fingerprint,
    verifier_kind,
    verifier_version,
    verifier_fingerprint,
    input_scope_hash
)
```

`gvr_count` est stocké comme métadonnée de validation mais ne fait pas
partie de l'identité de reuse. Le même `input_scope_hash` avec un `gvr_count`
différent indiquerait une corruption (hash cohérent mais nombre de sources
incohérent).

Un set existant avec cette identité exacte est réutilisé. `INSERT OR REPLACE`
est interdit.

### Immutabilité

Un Track Set publié est **immutable**. Aucune opération d'append, remove ou
merge n'est permise sur un track ou un set existant.

L'invalidation scientifique (nouvelle entrée, nouveau scope, nouvelle
configuration) produit un nouveau Track Set. Le set précédent reste intact.

La suppression référentielle utilise `ON DELETE CASCADE` : supprimer un
Track Set supprime ses tracks et observations.

### Justification

- chaque rebuild crée un nouveau set, les anciens restent intacts ;
- plusieurs configurations peuvent coexister (expérimentation) ;
- l'invalidation est simple : supprimer un set supprime ses tracks via
  CASCADE ;
- la reproductibilité est portée par le fingerprint et le scope_hash ;
- pas d'UPDATE/INSERT/MERGE sur des tracks existants ;
- cohérent avec tous les résultats publiés existants (Feature Sets, Match
  Results, GVRs) qui sont immutables après publication.

Le Track Builder v1 construit en mémoire, puis publie un set complet
dans une transaction. Aucun track n'est visible avant que le set entier soit
validé.

## Immutability / incrementality

Un Track publié dans un set est **immutable**.

L'incrémentalité est gérée par création de nouveaux sets :

1. nouvelles images → nouveaux Match Results → nouveaux GVR → nouveau
   Track Set ;
2. le set précédent reste valide et consultable ;
3. le futur Sparse SfM choisira quel set consommer.

Cette approche est cohérente avec la philosophie Lardon3D :

- résultats atomiques ;
- pas de destruction silencieuse ;
- reprise à frontière connue ;
- conservation de l'historique.

## Persistence

### Conceptual schema

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

### Schema invariants

**SQL-enforced :**

- `track_observations.PRIMARY KEY(track_set_id, feature_set_id, feature_index)`
  : dans un Track Set donné, une observation n'apparaît qu'une fois. Cela
  garantit qu'une observation scientifique appartient à au plus un Track dans
  ce set.
- `REFERENCES tracks(track_id) ON DELETE CASCADE` : l'observation appartient
  à un track existant ; supprimer le track supprime l'observation.
- `REFERENCES feature_sets(feature_set_id)` : le Feature Set existe.
- `CHECK(observation_count >= 2)` : minimum structurel.
- `UNIQUE(builder_kind, builder_version, parameter_fingerprint,
  verifier_kind, verifier_version, verifier_fingerprint,
  input_scope_hash)` sur `track_sets` : identité de reuse, empêche les
  doublons de set pour une même configuration et un même scope.
- `ON DELETE CASCADE` depuis `track_sets` : supprimer un set supprime tout.
- `CHECK(feature_index >= 0)` : borne inférieure de l'index.
- `UNIQUE(track_id, position_in_track)` : chaque position dans un track est
  unique. L'ordre est déterminé par le Track Builder lors de la publication.

**API-enforced :**

- `track_set_id` dans `track_observations` correspond au `track_set_id` du
  `track_id` parent. Le schéma ne comporte pas de FK composite (aucun
  précédent dans le codebase). L'API valide cette cohérence avant INSERT sous
  `BEGIN IMMEDIATE`.
- Une seule observation par image par track. L'API valide via jointure à
  `feature_sets.image_id`.
- `feature_index < feature_sets.feature_count`. L'API valide la borne
  supérieure.
- `observation_count` cohérent avec le nombre réel d'observations insérées.
- `track_count` cohérent avec le nombre réel de tracks insérés.
- `position_in_track` contigu à partir de 0 pour chaque track.

### Note sur la dénormalisation

`track_set_id` dans `track_observations` dénormalise une clé grandparent,
après le même pattern utilisé par `visual_index_memberships.visual_index_id`.
Le pattern parent-key-in-UNIQUE est déjà répandu dans le codebase. La cohérence
repose sur le chemin d'écriture unique du Track Builder et la validation API
sous transaction.

`track_observations.track_set_id` n'a pas de FK directe vers `track_sets`
pour éviter un second chemin CASCADE depuis `track_sets` vers
`track_observations` (le premier chemin passe par `tracks`). La cohérence
est garantie par l'API sous `BEGIN IMMEDIATE`.

## Provenance

### Track Set provenance

Chaque Track Set conserve :

- `builder_kind`, `builder_version`, `parameter_fingerprint` : configuration
  du Track Builder ;
- `verifier_kind`, `verifier_version`, `verifier_fingerprint` : configuration
  du Geometric Verifier consommé ;
- `input_scope_hash`, `gvr_count` : ensemble réel des GVR consommés.

Ces champs suffisent pour identifier la configuration scientifique complète
ayant produit le set.

### Edge provenance (deferred)

En v1, la provenance détaillée (quels GVR spécifiques ont contribué à quel
track individuel) n'est pas persistée. Les raisons :

- elle peut être reconstruite en comparant les memberships du set aux GVR
  disponibles ;
- une table `track_set_sources` volumineuse complexifie la DB sans bénéfice
  immédiat ;
- une future version du Track Builder pourra l'ajouter dans une migration
  ultérieure.

## Invalidation

### Invalidation scientifique

Un nouveau scope, une nouvelle configuration de verifier ou un nouveau
builder produit un **nouveau** Track Set avec une identité différente. Le set
 précédent reste intact et consultable. Aucune mutation silencieuse n'est
permise.

### Suppression référentielle

`ON DELETE CASCADE` s'applique :

- `track_sets` → `tracks` → `track_observations` : supprimer un set supprime
  tous ses tracks et observations ;
- `feature_sets` → (pas de CASCADE vers `track_observations`) : la FK utilise
  le comportement par défaut (NO ACTION). Supprimer un Feature Set référencé
  par une observation est interdit tant que l'observation existe.

## Atomic publication

L'unité persistante est le Track Set complet. La publication est une seule
transaction `BEGIN IMMEDIATE` contenant l'INSERT du set, de tous ses tracks
et de toutes ses observations.

- aucun track n'est visible avant le COMMIT du set entier ;
- un rollback ne laisse aucune ligne partielle ;
- le `created_at` du set est le timestamp de la transaction ;
- le `track_count` et `gvr_count` sont validés contre les INSERTs réels.

Le Track Builder v1 utilise le Task Runtime pour le checkpoint/reprise et le
Resource Governor pour l'admission. Le Model ne contient aucune
logique d'exécution.

## Resource bounds

- **Pas de plafond de longueur arbitraire** : le Model ne fixe pas de
  maximum sur le nombre d'observations par Track. Un projet avec N images
  peut produire des tracks de longueur jusqu'à N.
- **Lecture paginée** : `list_tracks` et `list_track_sets` utilisent une
  page de 64 entrées avec curseur.
- **Chargement borné** : load track by id charge les observations du track ;
  la taille est bornée naturellement par le nombre d'images dans le scope.
- **Pas de chargement complet du graphe** : aucune API ne charge tous les
  tracks et toutes les observations d'un projet en une seule fois.
- **Pas de matrice dense** : aucune matrice de co-visibilité N×N n'est
  matérialisée par le Model.

## Corruption handling

Le loader doit détecter :

- track absent (`track_id` référencé mais inexistant) ;
- observation invalide (`feature_set_id` inexistant) ;
- duplicate observation dans un même track set ;
- deux observations de la même image dans un même track ;
- `feature_index` hors bornes du Feature Set ;
- `observation_count` incohérent avec le nombre réel d'observations ;
- `track_set_id` dans `track_observations` ne correspondant pas au
  `track_set_id` du `track_id` parent ;
- `track_set` parent absent.

Toute corruption retourne `CORRUPT` sans résultat partiel.

## API

L'API publique implémente :

- `lardon3d_project_db_create_track_set()` — INSERT set + ses tracks +
  observations dans une seule transaction `BEGIN IMMEDIATE`.
- `lardon3d_project_db_load_track_set()` — SELECT par ID.
- `lardon3d_project_db_find_track_set()` — SELECT par identité exacte.
- `lardon3d_project_db_list_track_sets()` — SELECT paginé ORDER BY id,
  page 64.
- `lardon3d_project_db_load_track()` — SELECT par ID avec observations.
- `lardon3d_project_db_list_tracks()` — SELECT par set, paginé ORDER BY
  id, page 64.
- `lardon3d_project_db_find_track_by_observation()` — recherche par
  `(feature_set_id, feature_index)` dans un set donné.

La création valide en C : existence des Feature Sets, bornes des
`feature_index`, unicité des observations, unicité image par track,
`observation_count` cohérent, `track_set_id` cohérent. L'INSERT est
transactionnel.

## Explicitly out of scope

- Track Builder algorithmique (union-find, connected components) ;
- triangulation ;
- coordonnées 3D ;
- Essential matrix ;
- camera pose ;
- bundle adjustment ;
- sparse reconstruction / Sparse SfM ;
- reprojection error ;
- dense reconstruction ;
- Track optimization ou merge ;
- mutation de tracks existants ;
- co-visibilité (matrice ou calcul) ;
- sélection par timestamp ou "latest".

## Track rejected state

Le Model v1 ne persiste pas d'état Track rejected. Le Model représente des
Tracks structurellement valides (≥ 2 observations, cohérents). Le Track Builder
v1 décide quels candidats publier. Les candidats non publiés n'existent pas dans
le Model ; cette séparation reste la frontière scientifique figée.

## Versioning

Project DB v14 introduced the Track storage and v15 adds only durable Track
Builder task payload persistence. `builder_version` et `verifier_version`
décrivent indépendamment les contrats scientifiques.
Changer un algorithme n'impose une migration DB que si la représentation
persistante change.

# Base de données projet Lardon3D

## Vision

La base de données projet stocke les métadonnées de reconstruction et les relations entre les entités. Elle est conçue pour être légère, persistante et permettre la reprise après interruption.

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
- Identifiant unique
- Image source
- Image cible
- Score de similarité
- Source (visuelle, temporelle, etc.)

#### Verified Pair
- Identifiant unique
- Candidate pair source
- Statut (validée, rejetée)
- Métriques

#### Track
- Identifiant unique
- Observations
- Point 3D associé
- Qualité

#### Observation
- Identifiant unique
- Image
- Position 2D
- Descripteur
- Track parent

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
- Track → Observation (N:M)
- Observation → Image (1)
- Observation → Point3D (N:1)
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

## Schéma v5 implémenté

- `metadata(key PRIMARY KEY, value)` contient `schema_version=5` et
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

`AUTOINCREMENT` est volontairement limité à ces trois identités publiées. Il
empêche la réutilisation d'un ID issu d'une transaction validée même si sa ligne
maximale est supprimée plus tard. Le coût de `sqlite_sequence` est accepté pour
garantir qu'un futur Feature Store, match ou track ne voie jamais son identifiant
désigner un autre objet. Les IDs de transactions rollbackées ne sont pas
considérés publiés et peuvent être réutilisés.

Les indexes ajoutés en v4 sont `images(scanset_id,image_id)`, pagination réelle,
et `images(producer_task_id,image_id)`, recherche par tâche productrice. Le
SHA-256 et le chemin asset sont déjà indexés par leurs contraintes `UNIQUE`.

## Ouverture et migrations

Une DB vide reçoit directement le schéma v5 dans une transaction
`BEGIN IMMEDIATE`. Une DB v1 reçoit transactionnellement les colonnes nullable
`task_kind` et `task_kind_version`, puis les migrations v2→v3. Les anciennes lignes restent
`NULL/NULL`, sans type inventé et sans perte des projets, tâches, checkpoints ou
artefacts. Une interruption ou erreur provoque un rollback complet. Les DB v1,
v2, v3 et v4 sont migrées séquentiellement vers v5. Une version future est refusée et une DB contenant
des tables sans métadonnée de version est considérée corrompue. La fonction
interne de migration ne connaît que `0 → 4`, `1 → 2 → 3 → 4`,
`2 → 3 → 4` et `3 → 4`.

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

Configuration v5 : `foreign_keys=ON`, `journal_mode=DELETE`,
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

**IMPLEMENTED** — SQLite système, schéma v5 et migrations v1→v2→v3→v4→v5, identité
projet, transactions tâche+checkpoint, pagination de reprise et artefacts
génériques.

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

**NOT_YET_WIRED** — autosave à toutes les transitions, retry UI des sources
indisponibles, migration de la TUI legacy et réconciliation des fichiers
orphelins et Visual Index. Le Feature Store v1 est implémenté.

**PLANNED** — migrations v5+, dépendances d'artefacts, graphe géométrique et
reconstruction incrémentale.

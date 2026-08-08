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
- Scan Set → Image (N:M)
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
stockage. La future base réutilisera les mêmes règles de normalisation et de
validation ; elle ne stockera jamais les objets pthread, callbacks, pointeurs,
contrats ou réservations. Le fichier par tâche est une fondation, pas une
Project Database miniature.

## Statut

**NOT_YET_WIRED** — le modèle de checkpoint est prêt à être consommé.

**PLANNED** — schéma SQLite, migrations, transactions, inventaire d'artefacts,
chargement global et coordination avec le scheduler.

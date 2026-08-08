# Sous-système Candidate Pair

## Vision

Le sous-système Candidate Pair répond uniquement à la question :

> « Quelles paires d'images valent la peine d'être présentées au Matcher ? »

Il ne répond **PAS** à :

> « Ces images ont-elle réellement des correspondances ? »

et ne contient **aucune** validation géométrique.

## Frontières

```
Visual Index
    ↓
Candidate Pair Generator
    ↓
Candidate Pair persistence
    ↓
Matcher — HORS SCOPE de ce ticket
```

Le Matcher est un consommateur des paires persistées. Il n'est pas
implémenté dans ce sous-système.

## Invariants fondamentaux

| Invariant | Description |
|-----------|-------------|
| **Symétrie** | A,B == B,A |
| **Ordre canonique** | Stockage avec `image_id_a < image_id_b` |
| **Self-pairs interdits** | `image_id_a != image_id_b` (implicite via CHECK SQL) |
| **Unicité persistante** | `UNIQUE(image_id_a, image_id_b)` |
| **Résultat borné** | `top_k <= LARDON3D_VISUAL_INDEX_TOP_K_MAX = 256` par requête |
| **Déterminisme** | Mêmes entrées/configuration → mêmes paires dans le même ordre |
| **Idempotence** | Répétition sans duplication |
| **Persistance durable** | Paires persistées dans Project DB v8 |

## Modèle persistant

### Table `candidate_pairs` (Project DB v8)

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

### API

- `lardon3d_project_db_create_candidate_pair()` — INSERT avec canonicalisation
- `lardon3d_project_db_load_candidate_pair()` — SELECT par ID
- `lardon3d_project_db_find_candidate_pair()` — SELECT par (image_a, image_b)
- `lardon3d_project_db_list_candidate_pairs()` — SELECT paginé ORDER BY id

## Génération single-source

### Prototype

```c
Lardon3DVisualIndexResult lardon3d_candidate_pair_generate(
    const char *project_path, Lardon3DProjectDb *database,
    uint64_t visual_index_id, uint64_t source_feature_set_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    Lardon3DCandidatePairGenStats *stats);
```

### Algorithme

1. Charger le FeatureSet source
2. Obtenir `source_image_id`
3. Interroger le Visual Index avec `query_options`
4. Pour chaque candidat retourné :
   - Exclure les self-pairs
   - Canonicaliser l'ordre (image_a < image_b)
   - Chercher si la paire existe déjà (`find_candidate_pair`)
   - Créer si absente (`create_candidate_pair`)
5. Retourner les statistiques `{generated, skipped, queried}`

### Statistiques

```c
typedef struct {
  uint32_t generated_count;    // paires nouvellement créées
  uint32_t skipped_count;      // paires déjà existantes (idempotence)
  uint32_t queried_count;      // candidats retournés par le Visual Index
} Lardon3DCandidatePairGenStats;
```

## Score et provenance

Le score de retrieval et la provenance Visual Index ne sont **PAS**
persistés dans la table `candidate_pairs` pour les raisons suivantes :

- L'identité Candidate Pair est纯粹 géométrique : (image_a, image_b)
- Le score dépend de la configuration du Visual Index et peut changer
- Le Matcher calculera ses propres scores de matching
- La séparation des responsabilités est plus nette

Le score reste accessible via le Visual Index si nécessaire.

## Déterminisme

### Déterministe

- Mêmes entrées → mêmes paires
- Même ordre de sélection top-K
- Mêmes décisions de déduplication

### Non déterministe

- `created_at` (timestamp Unix, informatif uniquement)
- `candidate_pair_id` (AUTOINCREMENT, identifiant technique)

### Tie-breaks

En cas d'égalité de score dans le Visual Index, l'ordre est déterministe
selon l'implémentation LSH (ordre des Feature Sets).

## Invalidation

### Événements et impact

| Événement | Ce qui devient invalide | Ce qui reste réutilisable | Ce qui doit être recalculé |
|-----------|------------------------|--------------------------|---------------------------|
| Nouvelle image | Rien (incrémental) | Paires existantes | Nouvelles requêtes Visual Index |
| Nouveau FeatureSet | Rien | Paires existantes | Requête depuis ce FeatureSet |
| FeatureSet remplacé | Paires basées sur ce FeatureSet | Autres paires | Nouvelle requête depuis ce FeatureSet |
| Visual Index reconstruit | Toutes les paires (nouvelle config) | Rien | Tout recalcul |
| Configuration top-K modifiée | Rien (borné par requête) | Paires existantes | Nouvelles requêtes avec nouveau top_k |
| Filtre modifié | Rien | Paires existantes | Nouvelles requêtes avec nouveau filtre |
| Relance après interruption | Rien | Paires déjà persistées | Suite du traitement |

### Politique

L'invalidation est aussi locale que possible. On ne supprime jamais
toutes les paires du projet suite à une modification locale.

### Vérification par fingerprint

Le fingerprint permet de vérifier si une génération doit être recalculée :

```c
unsigned char fp_courant[32], fp_enregistre[32];
lardon3d_candidate_pair_generation_fingerprint(..., fp_courant);
// Si fp_courant != fp_enregistre → recalcul nécessaire
```

### Réutilisation

- Même fingerprint → résultat réutilisable
- Différent fingerprint → recalcul nécessaire
- Les paires existantes sont conservées même si le fingerprint change

## Batch projet

### Granularité

La génération batch traite un ensemble de FeatureSets de manière
bornée et déterministe.

### API

```c
Lardon3DVisualIndexResult lardon3d_candidate_pair_generate_batch(
    const char *project_path, Lardon3DProjectDb *database,
    uint64_t visual_index_id, uint64_t after_feature_set_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    Lardon3DCandidatePairGenStats *total_stats,
    uint64_t *last_feature_set_id);
```

### Algorithme

1. Lister les FeatureSets par pages de 64
2. Pour chaque FeatureSet :
   - Appeler `lardon3d_candidate_pair_generate()`
   - Accumuler les statistiques
   - Mettre à jour le curseur
3. Retourner les totaux et le dernier FeatureSet traité

### Ordre de traitement

Feature Sets traités en ordre croissant de `feature_set_id`.

### Déduplication

Une paire produite depuis plusieurs sources n'existe qu'une fois.
La deduplication est assurée par `find avant create`.

### Bornes

- Un seul FeatureSet traité à la fois
- Top-K borné par requête
- Mémoire bornée : allocation `top_k * sizeof(candidate)` par requête
- Pagination bornée (64 FeatureSets par page)

### Reprise

Le curseur `after_feature_set_id` permet la reprise après interruption.
La fonction retourne le dernier FeatureSet traité.

## Tâche durable

### Task Kind

`candidate_pair.generate` v1 — **IMPLEMENTED**.

### Unité de travail

Un FeatureSet source et sa requête Visual Index associée. Chaque séquence
traite un lot borné de FeatureSets (1 à 64 selon le contrat Governor),
page par page (PAGE_SIZE = 64).

### Checkpoint

Curseur `after_feature_set_id` persisté dans `candidate_pair_generate_tasks`.
Checkpoint sauvé après chaque lot via
`lardon3d_project_checkpoint_candidate_pair_generate_task()`.

### Reprise

Reprise idempotente : le champ `after_feature_set_id` est rechargé depuis la
DB, et les paires déjà persistées sont ignorées par `find avant create`.
À l'ouverture du projet, la tâche est automatiquement restaurée via la
registry production et resoumise à la queue.

### Intégration scheduler

La tâche utilise le scheduler générique via le pattern standard :
- Estimation immuable (128 Kio fixes, 256 Kio par item, lot 1–64)
- Réservation CPU + IO avant exécution
- `lardon3d_task_sequence_break()` entre chaque lot pour réadmission Governor
- Callback terminal checkpoint après `COMPLETED`/`FAILED`/`CANCELLED`
- Reconstruction depuis `Lardon3DProjectDbCandidatePairGenerateTask`

### API

```c
Lardon3DTask *lardon3d_project_create_candidate_pair_generate_task(
    Lardon3DAppState *state, uint64_t visual_index_id,
    const Lardon3DVisualIndexQueryOptions *query_options, uint64_t *task_id);
bool lardon3d_project_enqueue_candidate_pair_generate(
    Lardon3DAppState *state, uint64_t visual_index_id,
    const Lardon3DVisualIndexQueryOptions *query_options, uint64_t *task_id);
bool lardon3d_candidate_pair_generate_reconstruct(
    const Lardon3DTaskDurableSnapshot *snapshot, void *context,
    Lardon3DTaskKindBinding *binding);
```

## Concurrence

### Garantie actuelle

Le generator s'exécute sur un seul FeatureSet à la fois.
Le mutex DB protège les création concurrentes.

### Atomicité

Deux workers créant simultanément la même paire :
- Ne corrompent pas la DB
- Ne créent pas deux lignes (UNIQUE constraint)
- Retournent OK pour l'un, NOT_FOUND→create pour l'autre

### Limites

Le pattern `find + create` n'est pas atomique entre les deux appels.
Avec un seul worker, c'est suffisant. Avec plusieurs workers,
la contrainte UNIQUE assure l'unicité mais peut causer un retry.

## Bornes et ressources

### Top-K

`top_k <= 256` (LARDON3D_VISUAL_INDEX_TOP_K_MAX)

### Mémoire

Allocation par requête : `256 * sizeof(Lardon3DVisualIndexCandidate)`
≈ 256 * 40 = 10 240 octets (10 Kio).

### Complexité

- O(top_k) par requête (lectures Visual Index)
- O(1) par paire (DB write)
- Pas de structure O(N²)

## Fingerprint de génération

### Composants

Le fingerprint identifie une génération Candidate Pair unique :

```c
void lardon3d_candidate_pair_generation_fingerprint(
    uint64_t visual_index_id, uint64_t source_feature_set_id,
    const Lardon3DVisualIndexQueryOptions *query_options,
    unsigned char fingerprint[32]);
```

### Éléments inclus

- `visual_index_id` : Visual Index utilisé
- `source_feature_set_id` : Feature Set source
- `query_options->top_k` : nombre de candidats par requête
- `query_options->minimum_evidence_count` : filtre minimum
- `query_options->scanset_filter` : filtre ScanSet
- `query_options->exclude_same_asset` : exclusion même asset

### Éléments exclus (volontairement)

- `created_at` : informatif, pas fonctionnel
- `candidate_pair_id` : identifiant technique
- Ordre des Feature Sets traités en batch

### Stabilité

Le fingerprint est stable pour mêmes entrées et configuration.
Un changement de configuration produit un fingerprint différent.

### Relation avec l'invalidation

Un fingerprint différent signifie que le travail doit être recalculé.
Le même fingerprint signifie que le résultat peut être réutilisé.

### Relation avec la reprise

Le fingerprint permet de vérifier qu'une reprise utilise la même
configuration que l'originale.

## Limites connues

1. Score non persistant (par design)
2. La compaction de segments Visual Index n'est pas implémentée
3. Aucun Matcher consommateur des paires

## Statut

**IMPLEMENTED** — génération single-source, persistance,
canonicalisation, idempotence, batch projet, fingerprint et
réutilisation/invalidation.

**IMPLEMENTED** — tâche durable `candidate_pair.generate` v1
via le scheduler générique, avec estimation immuable,
checkpoint par curseur, reprise idempotente et intégration
dans la registry production.

## Relation avec le pipeline

Le Candidate Pair Generator est l'étape E du pipeline de reconstruction :

```
Feature Store (C)
    ↓
Visual Index (D)
    ↓
Candidate Pair Generator (E) ← CE DOCUMENT
    ↓
Matching (F) — HORS SCOPE
```

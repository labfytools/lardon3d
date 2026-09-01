# Reconstruction Layers

## Définition

Les **reconstruction layers** (couches de reconstruction) organisent le processus de reconstruction 3D en étapes séquentielles et indépendantes. Chaque couche traite un sous-ensemble du problème, produit un résultat validé, et passe le relais à la couche suivante. Cette approche séquentielle garantit la stabilité, l'observabilité et la reprise après interruption.

L'idée centrale est de décomposer la reconstruction en couches de complexité croissante, chaque couche étant atomique : elle lit un état d'entrée validé, calcule, et écrit un nouvel état validé.

## Statut

**ARCHIVE / PLANNED POUR CETTE ABSTRACTION GÉNÉRIQUE.** Le type de snapshot et
les cinq layers proposés ci-dessous ne sont pas une API implémentée. Les jalons
réels Track, Sparse SfM, BA, Phase H et MVS-M1 ont depuis été acquis par leurs
contrats canoniques distincts ; ils ne valident ni ce modèle générique de
layers, ni Dense complet, mesh, texturing ou viewer.

## Place dans le pipeline

```
Tracks ( entrée )
    ↓
Layer 0: Initial Reconstruction (triangulation basique)
    ↓
Layer 1: Bundle Adjustment (optimisation locale)
    ↓
Layer 2: Dense Reconstruction (dense matching)
    ↓
Layer 3: Mesh Generation (mailllage)
    ↓
Layer 4: Texturing (texture mapping)
    ↓
Résultat final validé
```

Chaque couche constitue une **frontière de séquence** : en cas d'interruption, la reprise se fait à la dernière frontière validée.

## Concepts clés

### Principes fondamentaux

1. **Atomicité** : chaque couche produit un résultat entièrement validé ou entièrement rejeté.
2. **Immutabilité** : une couche ne modifie jamais les résultats des couches précédentes.
3. **Idempotence** : relancer une couche avec les mêmes entrées produit le même résultat.
4. **Bornage** : chaque couche a un budget mémoire et temporel maximum.

### Couche 0 — Initial Reconstruction

| Aspect | Description |
|--------|-------------|
| **Entrée** | Tracks, caméras intrinsèques, co-visibilité |
| **Traitement** | Triangulation SVD, initialisation P3P, sélection du paire initial |
| **Sortie** | Nuage de points sparse, poses de caméras initiales |
| **Budget** | O(N²) dans le pire cas, typiquement O(N·K) où K = nombre moyen de tracks |

Étapes :
1. Sélection du **paire initial** (meilleure baseline angulaire + co-visibilité)
2. **Triangulation** des tracks du paire initial
3. **Rétro-projection** progressive des caméras adjacentes (grow)
4. Filtrage des points rejetés (erreur de reprojection > seuil)

### Couche 1 — Bundle Adjustment

| Aspect | Description |
|--------|-------------|
| **Entrée** | Nuage sparse, poses caméras, tracks |
| **Traitement** | Optimisation non-linéaire (LM ou Ceres-like), minimisation de l'erreur de reprojection |
| **Sortie** | Poses optimisées, points 3D affinés, historique de convergence |
| **Budget** | O(N·M) où N = points, M = observations |

Variantes :
- **Local BA** : optimisation d'un sous-ensemble de caméras voisines
- **Global BA** : optimisation simultanée de toutes les caméras et points
- **Incremental BA** : ajout progressif de caméras avec optimisation partielle

### Couche 2 — Dense Reconstruction

| Aspect | Description |
|--------|-------------|
| **Entrée** | Poses optimisées, images originales |
| **Traitement** | Semi-Global Matching (SGM), stereo matching, depth maps |
| **Sortie** | Depth maps par image, nuage de points dense |
| **Budget** | Très élevé en RAM : images × profondeur de recherche |

Approches :
- **Multi-View Stereo (MVS)** : matching dense entre paires de views
- **Patch-Based MVS** : regroupement de patches pour robustesse
- **Depth Map Fusion** : fusion des depth maps en nuage unifié

### Couche 3 — Mesh Generation

| Aspect | Description |
|--------|-------------|
| **Entrée** | Nuage de points dense |
| **Traitement** | Poisson reconstruction, Delaunay, ball pivoting |
| **Sortie** | Maillage triangulaire orienté |
| **Budget** | CPU intensif, mémoire proportionnelle au nombre de points |

### Couche 4 — Texturing

| Aspect | Description |
|--------|-------------|
| **Entrée** | Maillage, images originales, poses caméras |
| **Traitement** | Projection UV, visibilité caméra par face, blending |
| **Sortie** | Maillage texturé, atlas de texture |
| **Budget** | GPU intensif pour le rendu, RAM pour les textures |

### Frontières de séquence

Chaque couche produit un **snapshot** atomique :

```c
typedef struct {
    uint64_t layer_id;
    uint64_t sequence_id;
    int64_t  timestamp;
    uint32_t status;      // VALIDATED | REJECTED | PARTIAL
    size_t   data_size;
    void*    data;         // Pointeur vers le résultat sérialisé
} LayerSnapshot;
```

Les snapshots sont persistés sur disque et consultables par le viewer.

## Relations avec les autres modules

| Module | Relation |
|--------|----------|
| **Matching & Tracks** | Fournissent l'entrée de la couche 0. |
| **Geometric Constraints** | Utilisées dans toutes les couches pour filtrer et valider. |
| **Resource Governor** | Le gouverneur alloue les budgets pour chaque couche et contrôle la taille des lots. |
| **Task** | Chaque couche est une tâche candidate. Les couches sont séquencées par le runtime/Queue. |
| **Hardware Profile** | Les couches 2-4 peuvent exploiter le GPU si disponible. |
| **Image Catalog** | Les couches 2-4 lisent les images originales pour le dense matching et texturing. |
| **Viewer** | Le viewer affiche les snapshots des couches validées. |

## Contraintes de conception

- Une couche ne démarre que si la couche précédente a produit un snapshot VALIDATED.
- Les snapshots intermédiaires sont persistés permettant la reprise après crash.
- Chaque couche a un budget mémoire maximum imposé par le Resource Governor.
- La couche 1 (BA) est la plus critique : elle doit converger de manière déterministe.
- Les couches 2-4 peuvent être exécutées par lots (tiles) pour limiter la RAM.
- Le pipeline complet doit pouvoir être interrompu à n'importe quelle frontière.

## Terminologie

| Terme | Définition |
|-------|------------|
| **Layer** | Étape séquentielle et atomique de la reconstruction |
| **Frontière de séquence** | Point de reprise garanti entre deux couches |
| **Snapshot** | État persisté du résultat d'une couche |
| **Sparse reconstruction** | Nuage de points issu du matching (peu de points) |
| **Dense reconstruction** | Nuage de points issu du stereo matching (beaucoup de points) |
| **Bundle Adjustment** | Optimisation jointe des poses caméras et des points 3D |
| **Reprojection error** | Distance entre un point 3D projeté et son observation 2D |
| **Grow** | Ajout progressif de caméras à la reconstruction |

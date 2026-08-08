# Geometric Constraints

## Définition

Les **geometric constraints** (contraintes géométriques) sont des règles mathématiques qui encadrent la relation entre les images 2D et la scène 3D. Elles servent à valider, filtrer et optimiser les correspondances et la reconstruction en éliminant les solutions physiquement impossibles ou improbables.

Ces contraintes exploitent la géométrie projective des caméras, la structure de la scène et les propriétés des capteurs pour garantir la cohérence géométrique du pipeline.

## Statut

**PLANNED** — Concepts mathématiques fondamentaux, pas encore implémentés comme module distinct. Utilisés implicitement dans les futurs algorithmes de matching et reconstruction.

## Place dans le pipeline

```
Matching & Tracks
    ↓
Geometric Constraints (filtrage)
    ├── Fundamental Matrix Filter
    ├── Essential Matrix Filter
    ├── Homography Filter
    ├── Parallax Filter
    └── Visibility Constraints
    ↓
Reconstruction Layers (entrée validée)
```

Les contraintes géométriques agissent comme des filtres entre le matching brut et la reconstruction.

## Concepts clés

### 1. Fundamental Matrix (Matrice fondamentale)

La **matrice fondamentale** F encode la relation projective entre deux images pour une scène statique :

```
x'ᵀ F x = 0
```

où `x` et `x'` sont les coordonnées homogènes d'un point dans les deux images.

| Propriété | Valeur |
|-----------|--------|
| Dimensions | 3×3 |
| Rang | 2 |
| Degrés de liberté | 7 |
| Nombre minimum de points | 8 (ou 7 avec DLT) |

Usage :
- Vérification géométrique après matching brut
- Filtrage des outliers (points ne satisfaisant pas l'équation)
- Estimation de la configuration relative des caméras

### 2. Essential Matrix (Matrice essentielle)

La **matrice essentielle** E est la version calibrée de F, exprimée dans le repère caméra :

```
E = K'ᵀ F K
```

où `K` est la matrice intrinsèque de calibration.

| Propriété | Valeur |
|-----------|--------|
| Dimensions | 3×3 |
| Rang | 2 |
| Degrés de liberté | 5 (3 rotation + 2 translation) |
| Décomposition | E = t̂ R (rotation + translation) |

Usage :
- Extraction de la pose relative (R, t) entre deux caméras
- Triangulation des points 3D
- Estimation du baseline

### 3. Homography

L'**homographie** H encode la relation projective entre deux images d'un plan :

```
x' = H x
```

| Propriété | Valeur |
|-----------|--------|
| Dimensions | 3×3 |
| Degrés de liberté | 8 |
| Cas particulier | Scène plane ou rotation pure |

Usage :
- Détection de scènes planes (sols, murs, tableaux)
- Filtrage des matches sur plans dominants
- Warping et stitching d'images

### 4. Parallax Constraints (Contraintes de parallaxe)

La **parallaxe** est le décalage apparent d'un point entre deux images. Elle est liée à la distance focale, à la baseline et à la profondeur du point :

```
parallaxe = (f × baseline) / depth
```

| Type | Condition | Usage |
|------|-----------|-------|
| **Parallaxe minimale** | > seuil (ex: 0.5 pixel) | Éviter les dégénérescences (points à l'infini) |
| **Parallaxe maximale** | < seuil (ex: 100 pixels) | Éviter les erreurs de matching (trop de distortion) |
| **Parallaxe relative** | > baseline × sin(angle) | Garantir la qualité de triangulation |

### 5. Visibility Constraints (Contraintes de visibilité)

La **visibilité** encode quels points sont visibles depuis quelles caméras :

- **Culling** : un point derrière une caméra n'est pas visible
- **Occlusion** : un point peut être caché par un obstacle
- **Frustum** : un point hors du champ de vision n'est pas observable

```c
typedef struct {
    Vec3d point_3d;
    Vec3d camera_center;
    Vec3d camera_direction;
    float fov_horizontal;
    float fov_vertical;
    float near_plane;
    float far_plane;
} VisibilityConstraint;
```

### 6. Triangle Quality Constraints

La qualité des triangles de triangulation影响 la précision de la reconstruction :

| Métrique | Seuil recommandé | Description |
|----------|------------------|-------------|
| **Baseline angulaire** | > 5° | Angle entre les directions de vues |
| **Aspect ratio** | < 3.0 | Rapport longueur/largeur du triangle |
| **Epipolar distance** | < seuil pixel | Distance du point à la ligne épipolaire |
| **Reprojection error** | < 1-2 pixels | Erreur de reprojection après triangulation |

## Relations avec les autres modules

| Module | Relation |
|--------|----------|
| **Matching & Tracks** | Les contraintes filtrent les matches bruts pour ne garder que les géométries cohérentes. |
| **Visual Index** | Les candidats du Visual Index sont les entrées des contraintes géométriques. |
| **Reconstruction Layers** | Chaque couche applique les contraintes appropriées pour valider ses résultats. |
| **Resource Governor** | Le gouverneur peut ajuster les seuils de contrainte en fonction des ressources disponibles. |
| **Hardware Profile** | Le calcul des matrices fondamentales/essentielles est CPU-bound. |

## Contraintes de conception

- Les seuils géométriques (parallaxe minimale, erreur de reprojection) sont calibrables mais immuables pendant un traitement.
- L'estimation de F ou E utilise RANSAC avec un nombre d'itérations borné.
- Les homographies sont détectées automatiquement mais ne remplacent pas E pour les scènes non planes.
- Les contraintes de visibilité sont recalculées à chaque ajout de caméra.
- Les résultats de filtrage géométrique sont auditables (log des rejets avec raison).
- En cas d'incertitude, les contraintes sont conservatistes (rejeter plutôt qu'accepter).

## Terminologie

| Terme | Définition |
|-------|------------|
| **Fundamental Matrix** | Matrice 3×3 encodant la relation projective entre deux images |
| **Essential Matrix** | Version calibrée de F, dans le repère caméra |
| **Homographie** | Transformation projective entre deux vues d'un plan |
| **Epipolar geometry** | Géométrie des lignes épipolaires reliant deux vues |
| **Parallaxe** | Décalage apparent d'un point entre deux images |
| **RANSAC** | Random Sample Consensus, algorithme robuste d'estimation de modèles |
| **Inlier** | Point satisfaisant la contrainte géométrique |
| **Outlier** | Point ne satisfaisant pas la contrainte, rejeté |
| **DLT** | Direct Linear Transform, méthode d'estimation linéaire |
| **Bundle adjustment** | Optimisation globale minimisant l'erreur de reprojection |

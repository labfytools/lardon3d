# Persistance et base de données Lardon3D

## Vision

Lardon3D doit stocker les métadonnées de reconstruction dans une base de données persistante légère, probablement SQLite, tandis que les données numériques massives restent dans des fichiers/binaires adaptés.

## Principes fondamentaux

### Séparation logique/binaire
- État logique, relations, index → base persistante légère
- Données numériques massives → fichiers/artefacts binaires adaptés

### Cycle de publication
```
lot calculé
→ artefact temporaire
→ validation
→ publication atomique
→ transaction de métadonnées
→ état READY
```

### Règle de reprise
Une reprise ne considère jamais un artefact partiellement publié comme valide.

## Concepts de domaine

Les éléments suivants sont des concepts de domaine, PAS des tables SQL imposées :

- project
- scan_set
- image
- feature_set
- visual_signature
- candidate_pair
- verified_pair
- track
- observation
- camera
- pose
- point3d
- reconstruction_layer
- measurement
- document_source
- geometric_constraint
- artifact
- checkpoint

## Invariants

- Chaque publication est atomique
- Les artefacts partiels ne sont jamais considérés comme valides
- La reprise commence à la dernière frontière connue

## Statut : PLANNED (direction architecturale)

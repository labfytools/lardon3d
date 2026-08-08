# Viewer Lardon3D

## Vision

Le viewer est un composant graphique qui affiche les résultats de la reconstruction de manière interactive. Il est intégré à l'interface pour un usage confortable sur un seul écran, mais reste fonctionnellement séparé du moteur.

## Architecture

### Isolation fonctionnelle

Le viewer est un consommateur passif de snapshots validés. Il ne lit jamais les structures internes mutables du moteur.

```text
Moteur de reconstruction
    ↓
Snapshots atomiques et validés
    ↓
Viewer (consommation passive)
```

### Invariants

- Le viewer est un consommateur passif
- Aucune lecture directe de structures internes mutables
- Snapshots cohérents et validés uniquement
- Buffers visuels strictement bornés
- Si plusieurs snapshots deviennent obsolètes, privilégier le plus récent plutôt que créer un backlog
- Le calcul reste prioritaire
- Le viewer peut être désactivé ou ralentir sans bloquer le moteur
- Aucun thread graphique ne doit devenir une dépendance vitale du scheduler ou du gouverneur

### Frontière obligatoire

La frontière obligatoire est l'isolation fonctionnelle, pas nécessairement le processus Unix.

## Fonctionnalités

### Affichage
- Visualisation 3D des points, lignes et surfaces
- Navigation interactive (rotation, translation, zoom)
- Affichage des nuages de points, maillages et textures

### Interaction
- Sélection d'éléments
- Affichage d'informations détaillées
- Mesures et annotations

### Intégration
- Connexion au pipeline de reconstruction
- Affichage en temps réel de la progression
- Navigation dans l'historique des snapshots

## Limites actuelles

- Non implémenté
- Pas de dépendance vitale avec le moteur
- Peut être désactivé sans impact

## Statut : PLANNED (vision architecturale)

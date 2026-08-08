# Roadmap Lardon3D

## Direction générale

Lardon3D suit une feuille de route ordonnée qui privilégie la stabilité et la consolidation avant l'ajout de fonctionnalités complexes.

## Étapes terminées (DONE)

### Phase 1 : Fondations
- ✅ TUI modulaire avec ncursesw
- ✅ Gestion persistante des projets
- ✅ Import asynchrone et annulable
- ✅ Catalogue d'images et vues
- ✅ Moteur de tâches avec pause/reprise, annulation, checkpoints
- ✅ File FIFO avec sélection adaptative et backpressure
- ✅ Profil matériel et snapshots de ressources
- ✅ Resource Governor avec réservations opaques
- ✅ Intégration scheduler ↔ governor
- ✅ Sélection de la première tâche admissible

## Travaux d'infrastructure en cours (CURRENT FOUNDATION)

### Phase 2 : Consolidation
- 🔄 Documentation architecturale
- 🔄 Tests et validation
- 🔄 Optimisations mémoire

## Prochaines étapes décidées (NEXT)

### Phase 3 : Persistance
- 📋 Persistance des tâches et checkpoints
- 📋 Project Database v1 (SQLite)
- 📋 ScanSet et Image Catalog persistants

### Phase 4 : Pipeline avancé
- 📋 Feature Store
- 📋 Visual Index
- 📋 Candidate Pair Generator
- 📋 Matching et vérification géométrique
- 📋 Tracks et SfM

### Phase 5 : Reconstruction
- 📋 Reconstruction incrémentale
- 📋 MVS / dense
- 📋 Mesh
- 📋 Contraintes externes
- 📋 Consolidation

## Étapes futures (LATER)

### Phase 6 : Production
- ⏳ Viewer intégré
- ⏳ Publication live validée
- ⏳ Export multi-formats
- ⏳ Optimisations performances

### Phase 7 : Avancé
- ⏳ Priorités entre tâches
- ⏳ Pools de workers multiples (CPU/GPU/IO)
- ⏳ DAG de dépendances complet
- ⏳ Parallélisme inter-tâches

## Sujets exploratoires (RESEARCH)

- 🔬 Intégration avec des sources de données externes
- 🔬 Support de formats d'entrée variés
- 🔬 Optimisation pour machines à très faible mémoire
- 🔬 Distribution de calcul

## Principes directeurs

1. **Stabilité avant performance** : ne jamais saturer le système hôte
2. **Séquençage avant parallélisme** : lots adaptatifs et workers uniques d'abord
3. **Réservation atomique** : aucune exécution sans contrat valide
4. **Persistance progressive** : chaque résultat doit pouvoir être repris
5. **Documentation vivante** : la documentation suit le code, pas l'inverse

## Vérification

Cette roadmap est vérifiée contre l'état réel du code. Les fonctionnalités déjà implémentées ne sont pas marquées comme NEXT ou LATER.

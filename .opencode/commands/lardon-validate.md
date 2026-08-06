---
description: Compiler et valider Lardon3D avec les sanitizers pertinents
agent: lardon-tests
subtask: true
---

Valide le ticket courant : compilation propre avec Clang, tests Meson et
`git diff --check`. Ajoute ASan/UBSan pour les durées de vie et TSan pour la
concurrence. Ne modifie aucun fichier et rapporte précisément chaque résultat.

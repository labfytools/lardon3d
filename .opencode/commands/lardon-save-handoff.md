---
description: Sauvegarder l'état courant du ticket sans modifier les sources
agent: lardon-orchestrator
subtask: false
---

Sans sous-agent ni modification des sources, inspecte `git status`, le diff et
le handoff existant. Mets `.opencode/work/current_ticket.md` à jour avec les
rubriques prescrites. Résume les commandes et erreurs ; ne copie ni sources,
documentation, longs logs, secrets ou données utilisateur.

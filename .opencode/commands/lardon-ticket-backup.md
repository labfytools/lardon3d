---
description: Reprendre un ticket gratuit avec Nemotron après indisponibilité DeepSeek
agent: lardon-orchestrator
subtask: false
---

Lis d'abord `.opencode/work/current_ticket.md` et le diff. Si le handoff suffit,
ne refais pas l'exploration. Transmets seulement l'objectif, les fichiers, les
invariants, le travail restant et la prochaine action à lardon-build-backup.
Après reprise, appelle lardon-tests puis lardon-review ; concurrency uniquement
si pertinent. Mets à jour le handoff après chaque phase. Aucun modèle payant.

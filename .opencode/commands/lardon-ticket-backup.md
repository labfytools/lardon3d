---
description: Reprendre un ticket gratuit avec Nemotron après indisponibilité DeepSeek
agent: lardon-orchestrator
subtask: false
---

Lis d'abord `.opencode/work/current_ticket.md` et le diff. Si le handoff suffit,
ne refais pas l'exploration. Transmets seulement l'objectif, les fichiers, les
invariants, le travail restant et la prochaine action à lardon-build-backup.
Après reprise, appelle lardon-tests puis lardon-build-light en lecture seule pour
une première revue locale simple. N'appelle ni lardon-review ni
lardon-concurrency, car ils utilisent le même modèle que l'implémenteur. Si le
diff touche la concurrence, réserve sa revue au prochain passage Codex et
signale explicitement l'absence de revue indépendante forte. Mets à jour le
handoff après chaque phase. Aucun modèle payant.

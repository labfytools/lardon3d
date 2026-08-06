---
description: Reprise rapide du handoff avec Nemotron
agent: lardon-orchestrator
subtask: false
---

Reprends le ticket avec lardon-build-backup. Lis uniquement le handoff, le diff
et les fichiers qu'ils citent. Ne refais ni exploration ni architecture si les
informations suffisent. Après l'implémentation, utilise lardon-build-light en
lecture seule pour une revue locale simple. N'appelle ni lardon-review ni
lardon-concurrency : signale l'absence de revue indépendante forte et diffère
toute revue de concurrence sensible jusqu'au prochain passage Codex. Mets le
handoff à jour avec la prochaine action sûre.

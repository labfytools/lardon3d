---
description: Reprendre dans OpenCode le working tree laissé par Codex
agent: lardon-orchestrator
subtask: false
---

Lis le handoff et inspecte le diff laissé par Codex. Charge uniquement les
fichiers modifiés, sans refaire l'architecture. Lance lardon-review, puis
lardon-concurrency seulement si pertinent, puis lardon-tests. Mets le handoff à
jour avec les résultats et la prochaine action. Ne modifie pas les sources.

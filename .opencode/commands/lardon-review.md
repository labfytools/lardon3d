---
description: Lancer la seconde revue après validations vertes
agent: lardon-orchestrator
subtask: false
---

Lis le ticket durable et le diff ciblé. Lance `lardon-review`, puis
`lardon-concurrency` uniquement si le ticket touche un état partagé, une durée
de vie concurrente, le scheduler, les tâches ou le gouverneur. Lance ensuite
`lardon-docs` pour les contrats affectés. Agrège seulement les défauts, risques,
limites et prochaines corrections ; ne relance pas de build lourd en parallèle.

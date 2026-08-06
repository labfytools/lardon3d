---
description: Lancer une revue indépendante sans modification
agent: lardon-orchestrator
subtask: false
---

Sans modifier aucun fichier, identifie dans le handoff le modèle qui a
implémenté le diff. Si c'est lardon-build-backup ou Nemotron, demande à
lardon-build-light une première revue locale simple en lecture seule. N'appelle
ni lardon-review ni lardon-concurrency : signale explicitement l'absence de
revue indépendante forte et réserve toute revue de concurrence sensible au
prochain passage Codex.

Sinon, lance lardon-review. Ajoute lardon-concurrency uniquement si le diff
touche pthread, task, task_queue ou resource_governor. Agrège seulement les
défauts, risques et limites. Ne lance aucun autre agent.

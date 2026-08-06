---
description: Lancer une revue indépendante sans modification
agent: lardon-orchestrator
subtask: false
---

Sans modifier aucun fichier, lance lardon-review sur le diff courant. Lance
lardon-concurrency en sous-tâche indépendante uniquement si le diff touche
pthread, task, task_queue ou resource_governor. Agrège seulement les défauts,
risques et limites. Ne lance aucun autre agent.

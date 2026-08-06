---
description: Traiter un ticket Lardon3D avec exploration, architecture, tests et revue
agent: lardon-build
subtask: false
---

Traite ce ticket : $ARGUMENTS

Lis les instructions déjà chargées, inspecte Git et les fichiers concernés.
Utilise lardon-explore puis lardon-architect seulement si pertinents. Après leur
rapport, implémente seul et minimalement. Délègue la validation à lardon-tests,
puis la revue finale à lardon-review et obligatoirement à lardon-concurrency si
task, task_queue, resource_governor ou pthread sont touchés. Aucun sous-agent
imbriqué, commit, push ou ajout de scan3d/.

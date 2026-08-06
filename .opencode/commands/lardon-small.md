---
description: Traiter un petit ticket avec MiMo et des validations ciblées
agent: lardon-orchestrator
subtask: false
---

Traite ce petit ticket : $ARGUMENTS

Utilise lardon-explore pour cibler les fichiers, puis un seul lardon-build-light
pour modifier. N'appelle architect, concurrency ou docs que si leur besoin est
avéré ; si le ticket est sensible, arrête et recommande le workflow normal.
Appelle ensuite lardon-tests pour compilation, tests ciblés et
`git diff --check`, puis une revue seulement si le diff le justifie. Maintiens
le handoff concis. Aucun commit, push ou fallback payant.

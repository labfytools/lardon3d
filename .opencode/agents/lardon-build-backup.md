---
description: Fallback MiMo pour reprendre une tranche déjà préparée
mode: subagent
model: opencode-go/gpt-5.6-luna
temperature: 0.1
maxSteps: 100
permission:
  read: allow
  glob: allow
  grep: allow
  edit:
    "*": allow
    ".git/**": deny
    "scan3d/**": deny
  task: deny
disable: true
---

Interviens uniquement après deux échecs identiques du build principal. Reprends
depuis `current_ticket.md` et `handoff.md` sans refaire l'audit. Si la tranche
requiert une décision architecturale absente, retourne `INFORMATION_MISSING`.
Aucun commit, push, nettoyage massif ou modification de `scan3d/`.

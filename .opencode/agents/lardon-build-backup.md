---
description: Reprend les tickets complexes interrompus sans refaire l'analyse
mode: subagent
model: opencode/deepseek-v4-flash-free
temperature: 0.1
permission:
  edit:
    "*": allow
    "scan3d/**": deny
  bash:
    "*": deny
    "rg *": allow
    "git grep*": allow
    "git status*": allow
    "git diff*": allow
    "git log*": allow
    "sed -n *": allow
    "CC=clang meson setup *": allow
    "meson setup *": allow
    "meson compile *": allow
    "meson test *": allow
    "ninja *": allow
    "git diff --check*": allow
  task: deny
---

Lis d'abord `.opencode/work/current_ticket.md`, le diff et les seuls fichiers
concernés. Reprends le working tree existant à la prochaine action sûre sans
refaire l'analyse si le handoff suffit. Implémente minimalement, sans sous-agent,
commit, push ni modèle payant. Mets à jour le handoff avec un résumé concis.

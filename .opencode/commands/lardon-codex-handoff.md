---
description: Préparer une reprise courte dans Codex CLI
agent: lardon-orchestrator
subtask: false
---

Mets le handoff à jour depuis le statut et le diff, sans modifier les sources.
Vérifie d'abord `codex --help`. Produis ensuite un prompt court copiable qui
demande à Codex de lire AGENTS.md, le handoff, le diff et seulement les fichiers
cités, puis de reprendre à la prochaine action sûre. Ne lance pas Codex, n'invente
aucune option, ne demande aucune clé et n'inclus aucun secret.

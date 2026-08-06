---
description: Traiter un ticket gratuit avec contexte progressif et handoff
agent: lardon-orchestrator
subtask: false
---

Traite ce ticket : $ARGUMENTS

1. Initialise `.opencode/work/current_ticket.md` avec les rubriques prescrites.
2. Demande à lardon-explore les seuls fichiers et documents utiles.
3. Charge uniquement ce contexte. Appelle lardon-architect seulement pour une
   abstraction importante, puis transmets un résumé concis à lardon-build.
4. Après l'implémentation, appelle lardon-tests une fois, puis lardon-review une
   fois. Ajoute lardon-concurrency uniquement pour du code concurrent.
5. Mets le handoff à jour après chaque phase avec conclusions, commandes,
   erreurs et prochaine action, sans documentation, sources ou logs complets.

Un seul agent écrit les sources. Aucun sous-agent imbriqué, commit, push,
fallback payant ou modification de scan3d/.

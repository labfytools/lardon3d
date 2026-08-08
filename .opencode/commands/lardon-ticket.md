---
description: Traiter un ticket complet par phases durables
agent: lardon-orchestrator
subtask: false
---

Traite ce ticket en mode long run par phases :

$ARGUMENTS

Initialise `current_ticket.md`, audite uniquement le périmètre utile, puis
enchaîne architecture, tranches d'implémentation et tests ciblés. Une seule
écriture et une seule validation lourde à la fois. Après le vert complet,
impose review, concurrency si concernée, docs, corrections et revalidation.
Mets à jour la mémoire durable après chaque phase et prépare `handoff.md` avant
toute fin de contexte. Aucun commit, push ou changement de `scan3d/`.

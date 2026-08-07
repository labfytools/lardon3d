---
description: Traiter un ticket complet avec contexte progressif et handoff
agent: lardon-orchestrator
subtask: false
---

Traite le ticket suivant :

$ARGUMENTS

Le contexte permanent de Lardon3D est déjà chargé via
`.opencode/context.md`.

Considère également AGENTS.md et le handoff courant comme déjà disponibles.
Ne les relis que lorsqu'une information précise manque.

N'effectue jamais :

- d'inventaire global du dépôt ;
- de `glob "*"` ;
- de `find` sur tout le dépôt ;
- de lecture complète de toute la documentation.

Procédure :

1. Initialiser `.opencode/work/current_ticket.md` avec les rubriques prescrites.
2. Utiliser `lardon-explore` avec une recherche strictement ciblée.
3. Identifier uniquement :
   - les fichiers à modifier ;
   - les dépendances directes ;
   - les tests concernés ;
   - les documents réellement utiles.
4. Charger uniquement ce contexte.
5. Appeler `lardon-architect` uniquement si une évolution d'architecture est
   réellement nécessaire.
6. Transmettre à `lardon-build` un résumé concis contenant :
   - objectif ;
   - contraintes ;
   - API concernées ;
   - invariants ;
   - fichiers ;
   - validations attendues.
7. Après l'implémentation :
   - appeler `lardon-tests` une seule fois ;
   - appeler `lardon-review` une seule fois ;
   - appeler `lardon-concurrency` uniquement si le ticket touche :
     - pthread ;
     - mutex ;
     - condition variables ;
     - scheduler ;
     - task_queue ;
     - resource_governor ;
     - code concurrent.
8. Mettre à jour le handoff après chaque phase importante.

Le handoff doit rester court et contenir uniquement :

- objectif ;
- contraintes ;
- fichiers concernés ;
- travail effectué ;
- validations exécutées ;
- erreurs rencontrées ;
- limites restantes ;
- prochaine action sûre.

Règles :

- un seul agent écrit les sources ;
- aucun sous-agent imbriqué ;
- aucun agent inutile ;
- aucun modèle payant ;
- aucun commit ;
- aucun push ;
- aucune modification de `scan3d/`.

Privilégier systématiquement :

- le plus petit contexte possible ;
- le moins d'agents possible ;
- une seule compilation complète ;
- une seule revue ;
- une seule analyse de concurrence lorsque nécessaire.

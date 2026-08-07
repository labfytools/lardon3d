---
description: Traiter un petit ticket avec MiMo et des validations ciblées
agent: lardon-orchestrator
subtask: false
---

Traite le petit ticket suivant :

$ARGUMENTS

Le contexte permanent de Lardon3D est déjà chargé via `.opencode/context.md`.

Ne relis AGENTS.md, l’overview ou la documentation générale que si une information
précise manque.

Ce workflow est réservé aux modifications locales, simples et clairement
délimitées.

N’effectue jamais :

- d’inventaire global du dépôt ;
- de `glob "*"` ;
- de `find` sur tout le dépôt ;
- de lecture complète de la documentation ;
- de refactoring d’architecture non demandé.

Procédure :

1. Lire `.opencode/work/current_ticket.md` s’il existe et s’il concerne ce ticket.
2. Utiliser `lardon-explore` avec une recherche strictement ciblée.
3. Identifier uniquement :
   - les fichiers à modifier ;
   - les dépendances directes ;
   - les tests concernés ;
   - les éventuels documents à mettre à jour.
4. Vérifier que le ticket convient au mode léger.
5. Utiliser un seul `lardon-build-light` pour l’implémentation.
6. Appeler `lardon-tests` une seule fois après l’implémentation pour :
   - compiler avec Clang ;
   - exécuter les tests ciblés ;
   - exécuter `git diff --check`.
7. Demander une revue uniquement si le diff le justifie réellement.
8. Mettre à jour le handoff avec un résumé concis.

N’appelle pas `lardon-architect`, `lardon-concurrency` ou `lardon-docs` sauf si
leur intervention est manifestement nécessaire.

Arrête le workflow léger et recommande `/lardon-ticket` ou Codex si le ticket
touche notamment :

- une API publique structurante ;
- `task`, `task_queue` ou `resource_governor` ;
- pthread, mutex ou condition variables ;
- la durée de vie complexe de ressources ;
- la persistance ou le rollback ;
- le scheduler, un futur DAG ou les réservations ;
- un refactoring réparti sur plusieurs modules ;
- une fondation sensible du projet.

Dans ce cas, ne tente aucune modification risquée avec MiMo.

Le handoff doit rester court et contenir seulement :

- objectif ;
- fichiers concernés ;
- travail effectué ;
- validations ;
- erreurs ;
- limites ;
- prochaine action sûre.

Ne fais aucun commit.
Ne fais aucun push.
N’utilise aucun modèle payant.
Ne modifie jamais `scan3d/`.

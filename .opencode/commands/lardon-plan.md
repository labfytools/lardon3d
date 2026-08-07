---
description: Planifier un ticket en lecture seule avec un contexte minimal
agent: lardon-orchestrator
subtask: false
---

Planifie le ticket suivant sans modifier les sources :

$ARGUMENTS

Le contexte permanent est déjà injecté depuis `.opencode/context.md`.

Pour cette commande :

- n'appelle aucun sous-agent ;
- ne lance aucune commande Bash ;
- ne modifie aucun fichier sauf
  `.opencode/work/current_ticket.md`.

Ne lance jamais :

- `pwd`
- `ls`
- `find`
- glob
- inventaire du dépôt
- compilation
- tests

Réponds uniquement à partir :

- du contexte permanent ;
- du texte du ticket ;
- de `.opencode/work/current_ticket.md` s'il existe et concerne déjà ce ticket.

Si une information manque, indique simplement les deux ou trois fichiers qui
devront être lus lors d'une future exploration ciblée.

Ne tente jamais de les découvrir toi-même.

Choisis un seul ticket.

Ne fusionne jamais plusieurs tickets.

Ne propose jamais :

- de métrique ;
- de compteur ;
- de diagnostic ;
- de nouvelle API ;
- d'état TUI ;
- de fonctionnalité annexe ;
- de refactoring non demandé.

Le plan doit uniquement couvrir le périmètre demandé.

Dans la section **Agents nécessaires** :

- n'ajoute que les agents réellement utiles ;
- si le ticket touche `task`, `task_queue`, le scheduler,
  `resource_governor`, pthread, mutex, variables de condition,
  pause, reprise, annulation, réservations ou tout état partagé,
  inclure obligatoirement `lardon-concurrency`,
  même si aucun nouveau thread n'est créé.

Écris `.opencode/work/current_ticket.md`
une seule fois avec exactement cette structure :

Le handoff représente uniquement le ticket courant.

Si un nouveau ticket est demandé,
remplacer entièrement `.opencode/work/current_ticket.md`.

Ne jamais conserver plusieurs tickets simultanément.

Ne jamais demander quel ticket utiliser.

# Ticket

## Objectif

## Contraintes

## Fichiers probablement concernés

## Documents nécessaires

## Plan

## Agents nécessaires

## Agents non nécessaires

## Tests futurs

## Risques

## Prochaine action sûre

Contraintes :

- une seule occurrence de chaque rubrique ;
- aucun doublon ;
- aucun code inventé ;
- aucune API inventée ;
- aucune fonctionnalité hors périmètre ;
- aucune modification incrémentale répétée ;
- 80 lignes maximum.

Après l'écriture :

1. relire le fichier une seule fois ;
2. vérifier uniquement :
   - structure ;
   - absence de doublons ;
   - cohérence ;
3. terminer immédiatement.

Ne fais aucun commit.

Ne fais aucun push.

N'utilise aucun modèle payant.

Ne modifie jamais `scan3d/`.

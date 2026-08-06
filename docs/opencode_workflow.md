# Workflow OpenCode et Codex

Ce workflow minimise le contexte tout en conservant un handoff fiable. Le
fichier `.opencode/work/current_ticket.md` est local et ignoré par Git. Il ne
contient que l'objectif, les contraintes, fichiers et documents utiles, plan,
travail terminé et restant, fichiers modifiés, commandes, tests, décisions,
erreurs, modèle actif et prochaine action sûre.

## A. Ticket gratuit normal

Lancer `/lardon-plan`, puis `/lardon-ticket`. North orchestre et explore via des
agents distincts, puis DeepSeek implémente. Architect et concurrency ne sont appelés que si le
périmètre l'exige. Tests et revue interviennent une seule fois après le code.

## B. DeepSeek retourne 503

Lancer `/lardon-save-handoff`, puis `/lardon-resume-backup`. Nemotron lit le
handoff et le diff, reprend la prochaine action et ne refait pas l'analyse si
les informations suffisent.

## C. DeepSeek et Nemotron sont indisponibles

Lancer `/lardon-resume-light` uniquement si le travail restant est petit et
local. Pour une fondation sensible, conserver le handoff et attendre DeepSeek
ou Codex ; ne pas forcer MiMo à réécrire l'architecture.

## D. Petit ticket avec MiMo

Lancer `/lardon-small`. L'exploration reste ciblée, MiMo est le seul auteur,
puis les validations ciblées et `git diff --check` sont exécutées. Basculer vers
le workflow normal si la complexité dépasse une correction locale.

## E. Quota Codex renouvelé

GPT-5.6 Sol visible dans OpenCode est une offre Zen payante, pas une voie Codex
confirmée. Lancer `/lardon-codex-handoff`, copier le prompt court produit dans
Codex CLI, puis utiliser Codex directement. Aucune clé n'est copiée entre les
outils.

## F. Retour depuis Codex

Lancer `/lardon-resume-from-codex`. OpenCode lit le handoff et le diff laissé
par Codex, puis effectue revue et tests sans refaire l'architecture. Le handoff
est ensuite actualisé.

## G. GPT-5.6 Sol direct dans OpenCode

Ce scénario est désactivé : Q1 n'est pas confirmé. Il ne pourra être ajouté que
si une route ChatGPT/Codex officiellement supportée, sans facturation Zen ou API
distincte, et une consommation du quota Codex sont toutes prouvées.

## H. Authentification directe avec quota inconnu

Le cas Q2 n'a pas été observé. S'il apparaît plus tard, aucune utilisation ne
doit être automatique : documenter le fournisseur et le risque, puis obtenir une
décision explicite avant une commande expérimentale.

## I. Fallback payant interdit

La configuration ne référence que des modèles explicitement gratuits. Une panne
de tous les secours gratuits arrête le ticket en conservant le handoff. Elle ne
doit jamais sélectionner `big-pickle`, GPT-5.6 Sol ou un autre modèle tarifé ou
de statut inconnu.

## Chargement progressif

1. Lire AGENTS.md et l'overview injectés par la configuration.
2. Faire identifier par explore les seuls fichiers et documents utiles.
3. Charger ces documents seulement ; appeler architect si une abstraction est
   touchée.
4. Transmettre à l'implémenteur un résumé concis, jamais les documents complets.
5. Appeler uniquement les validations et revues pertinentes.

Pour une reprise, commencer par le handoff et le diff. Les sorties intermédiaires
ne conservent que conclusions, risques, fichiers, commandes, erreurs et
décisions ; les longs logs et les sources complètes sont exclus.

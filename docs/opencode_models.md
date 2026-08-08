# Modèles OpenCode (DÉPRÉCIÉ)

> **Ce document est déprécié.** Il a été remplacé par la documentation structurée dans :
> - [.opencode/context.md](../.opencode/context.md) (contexte permanent)
> - [.opencode/agents/](../.opencode/agents/) (définitions des agents)
>
> Ce document est conservé uniquement pour la traçabilité historique.

---

Inventaire effectué le 6 août 2026 avec
`opencode models --refresh --verbose`. Le seul fournisseur connecté exposé est
`opencode` via OpenCode Zen. Aucun modèle local n'est apparu. Les catégories
ci-dessous reposent sur le nom explicite et les champs `cost` retournés par le
CLI, pas sur une supposition liée à la famille du modèle.

## Modèles gratuits détectés

Sept modèles sont explicitement nommés « Free » et annoncent un coût nul en
entrée, sortie et cache :

- `opencode/deepseek-v4-flash-free` : agent principal `lardon-build` ; repli
  recommandé `opencode/nemotron-3-ultra-free` en cas de saturation 503.
- `opencode/nemotron-3-ultra-free` : architecture, concurrence, revue et repli
  de l'agent principal.
- `opencode/mimo-v2.5-free` : exploration rapide et validations.
- `opencode/laguna-s-2.1-free` : testé comme reviewer sur Lardon3D ; résultats
  jugés insuffisants. Non utilisé dans les rôles actifs.
- `opencode/ling-3.0-flash-free` : documentation.
- `opencode/longcat-2.0-free` : disponible, non configuré actuellement.

## Gratuité non confirmée

`opencode/big-pickle` annonce un coût nul dans les métadonnées du CLI, mais son
nom ne le qualifie pas explicitement de gratuit. Il reste volontairement non
configuré tant que les conditions de son offre ne sont pas confirmées.

## Modèles payants volontairement exclus

Les modèles suivants annoncent un coût non nul et ne sont référencés par aucun
agent ni par la configuration principale :

- Claude : `claude-fable-5`, `claude-haiku-4-5`, `claude-opus-4-1`,
  `claude-opus-4-5`, `claude-opus-4-6`, `claude-opus-4-7`,
  `claude-opus-4-8`, `claude-opus-5`, `claude-sonnet-4`,
  `claude-sonnet-4-5`, `claude-sonnet-4-6`, `claude-sonnet-5`.
- DeepSeek : `deepseek-v4-flash`, `deepseek-v4-pro`.
- Gemini : `gemini-3-flash`, `gemini-3.1-pro`, `gemini-3.5-flash`,
  `gemini-3.5-flash-lite`, `gemini-3.6-flash`.
- GLM : `glm-5`, `glm-5.1`, `glm-5.2`.
- GPT : `gpt-5`, `gpt-5-codex`, `gpt-5-nano`, `gpt-5.1`,
  `gpt-5.1-codex`, `gpt-5.1-codex-max`, `gpt-5.1-codex-mini`,
  `gpt-5.2`, `gpt-5.2-codex`, `gpt-5.3-codex`,
  `gpt-5.3-codex-spark`, `gpt-5.4`, `gpt-5.4-mini`, `gpt-5.4-nano`,
  `gpt-5.4-pro`, `gpt-5.5`, `gpt-5.5-pro`, `gpt-5.6-luna`,
  `gpt-5.6-sol`, `gpt-5.6-terra`.
- Grok : `grok-4.5`, `grok-build-0.1`.
- Kimi : `kimi-k2.5`, `kimi-k2.6`, `kimi-k2.7-code`, `kimi-k3`.
- MiniMax : `minimax-m2.5`, `minimax-m2.7`, `minimax-m3`.
- Qwen : `qwen3.5-plus`, `qwen3.6-plus`.

Tous ces identifiants ont le préfixe fournisseur `opencode/`. Aucun basculement
automatique vers eux n'est autorisé. Si tous les modèles gratuits adaptés sont
indisponibles, interrompre le travail plutôt que choisir un modèle payant.

## Modèles locaux

Aucun fournisseur ni modèle local n'a été détecté dans la configuration
OpenCode actuelle.

## Optimisation du quota

L’agent par défaut est `lardon-orchestrator` sur MiMo V2.5 Free.

Le contexte permanent est chargé depuis `.opencode/context.md`. L’orchestrateur
ne relit pas automatiquement AGENTS.md, l’overview ni toute la documentation
d’architecture. Il utilise uniquement le handoff courant et les documents
directement liés au ticket.

MiMo assure :

- l’orchestration ;
- l’exploration ciblée ;
- les tests ;
- les petits tickets.

DeepSeek V4 Flash Free est réservé à l’implémentation complexe. Il ne sert ni à
l’orchestration, ni à l’exploration, ni aux tests, ni à la documentation.

Nemotron 3 Ultra Free assure :

- les reprises après indisponibilité de DeepSeek ;
- l’architecture ;
- la concurrence ;
- la revue générale.

Ling 3.0 Flash Free est réservé à la documentation.

North Mini Code Free n’est plus utilisé dans les rôles actifs. Il reste mentionné
dans l’inventaire historique comme modèle testé puis écarté en raison de résultats
jugés insuffisants et trop erratiques sur Lardon3D.

Les commandes `lardon-plan`, `lardon-small`, `lardon-ticket`,
`lardon-ticket-backup` et les commandes de reprise utilisent un contexte
progressif.

Elles ne lisent jamais automatiquement tout le dépôt ni toute l’architecture.
Elles transmettent uniquement :

- objectif ;
- contraintes ;
- fichiers concernés ;
- API ;
- invariants ;
- risques ;
- tests requis ;
- prochaine action sûre.

Les rapports de succès restent courts. Le handoff ignoré
`.opencode/work/current_ticket.md` permet une reprise sans rejouer l’exploration
ou l’analyse déjà terminée.

Nemotron ne relit jamais sa propre implémentation lorsqu’il a servi de backup.

Dans ce cas :

- MiMo fournit seulement une première revue locale simple ;
- la revue de concurrence sensible est différée jusqu’au prochain passage Codex ;
- le workflow signale explicitement qu’aucune revue indépendante forte n’a eu
  lieu.

Aucun modèle payant n’est utilisé automatiquement.

## Chaîne de secours

La voie normale est DeepSeek V4 Flash Free pour l'implémentation. En cas de 503,
saturation, quota épuisé ou interruption, sauvegarder le handoff et reprendre
avec Nemotron 3 Ultra Free. Si Nemotron est indisponible, MiMo V2.5 Free est
limité aux petits changements ; il doit refuser une fondation sensible.

GPT-5.6 Sol n'est pas actif dans OpenCode, car l'identifiant visible passe par
Zen et annonce un tarif. Pour utiliser le quota ChatGPT/Codex, la voie normale
reste Codex CLI via `lardon-codex-handoff`, puis
`lardon-resume-from-codex`. Aucun modèle payant n'est un fallback.

## GPT-5.6 Sol dans OpenCode

Vérification locale du 6 août 2026 avec OpenCode 1.18.13 :

- identifiant exact : `opencode/gpt-5.6-sol` ;
- fournisseur : `opencode`, affiché comme OpenCode Zen ;
- endpoint déclaré : `https://opencode.ai/zen/v1` ;
- authentification active : credential Zen de type `api` ;
- clé OpenAI API séparée : non requise par cette route Zen, mais le credential
  Zen reste nécessaire ;
- OAuth ChatGPT/Codex : non détecté ;
- plugin Codex OAuth : non installé ;
- OpenCode Zen utilisé : oui ;
- tarif affiché : entrée 5, sortie 30, cache lecture 0,5 et écriture 6,25 dans
  les unités tarifaires retournées par OpenCode ; tarifs supérieurs au-delà du
  palier de contexte indiqué ;
- facturation distincte du quota Codex : route Zen tarifée, donc à considérer
  distincte ;
- quota Codex partagé : non confirmé et aucune preuve locale ne l'indique ;
- test minimal : non exécuté, car la route identifiée est payante ;
- classement : **Q3 — GPT-5.6 Sol fourni par OpenCode Zen** ;
- politique : aucun agent, commande active ou fallback Sol dans OpenCode.

La preuve locale combine `opencode models --refresh --verbose`, qui expose le
fournisseur, l'endpoint et le coût, et `opencode providers list`, qui expose un
seul credential OpenCode Zen. La configuration globale ne déclare aucun
fournisseur OpenAI ni plugin d'authentification Codex. Aucun secret n'a été lu
ou reproduit dans ce document.

## Actualisation

Les offres changent. Réexécuter :

```sh
opencode models --refresh --verbose
```

Comparer les identifiants, le fournisseur et surtout les champs `cost`. Ne pas
déduire la gratuité du nom seul. Mettre ensuite à jour ce document et chaque
frontmatter `model:` sous `.opencode/agents/`, puis valider la configuration
avec `opencode debug config`.

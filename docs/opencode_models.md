# Inventaire des modèles OpenCode

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
- `opencode/nemotron-3-ultra-free` : architecture et concurrence ; repli de
  l'agent principal.
- `opencode/north-mini-code-free` : exploration rapide et validations.
- `opencode/laguna-s-2.1-free` : revue indépendante ; repli économique pour les
  analyses générales.
- `opencode/ling-3.0-flash-free` : documentation.
- `opencode/longcat-2.0-free` : disponible, non configuré actuellement.
- `opencode/mimo-v2.5-free` : disponible, non configuré actuellement.

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

## Actualisation

Les offres changent. Réexécuter :

```sh
opencode models --refresh --verbose
```

Comparer les identifiants, le fournisseur et surtout les champs `cost`. Ne pas
déduire la gratuité du nom seul. Mettre ensuite à jour ce document et chaque
frontmatter `model:` sous `.opencode/agents/`, puis valider la configuration
avec `opencode debug config`.

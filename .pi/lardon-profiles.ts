import type {
  ExtensionAPI,
  ExtensionContext,
} from "@earendil-works/pi-coding-agent";

type ProfileName =
    | "local"
    | "ctx"
    | "review"
    | "mem"
    | "full"
    | "codex";

interface Profile {
  provider: string;
  model: string;
  thinking: "off" | "low";
  tools: string[];
}

const profiles: Record<ProfileName, Profile> = {
  /*
   * Default mode.
   * Qwen3 local, read-only, enough for normal repository exploration.
   */
  local: {
    provider: "local-lardon",
    model: "lardon-local",
    thinking: "off",
    tools: [
      "read",
      "grep",
      "find",
      "ls",
    ],
  },

  review: {
   provider: "local-lardon",
    model: "lardon-local",
    thinking: "off",
    tools: [
      "read",
      "grep",
      "find",
      "ls",
      "lsp_diagnostics",
    ],
  },

  /*
   * Local + context-mode through the compact MCP proxy.
   * No direct ctx_* tools are exposed.
   */
  ctx: {
    provider: "local-lardon",
    model: "lardon-local",
    thinking: "off",
    tools: [
      "read",
      "grep",
      "find",
      "ls",
      "lsp_diagnostics",
      "subagent",
      "mcp",
    ],
  },

  /*
   * Local + Hermes search.
   * memory_search is intentionally not enabled in the default profile
   * because small local models tend to invoke it too eagerly.
   */
  mem: {
    provider: "local-lardon",
    model: "lardon-local",
    thinking: "off",
    tools: [
      "read",
      "grep",
      "find",
      "ls",
      "memory_search",
    ],
  },

  /*
   * Local with both optional facilities.
   * Use only when a task genuinely needs memory + context-mode.
   */
  full: {
    provider: "local-lardon",
    model: "lardon-local",
    thinking: "off",
    tools: [
      "read",
      "grep",
      "find",
      "ls",
      "lsp_diagnostics",
      "subagent",
      "mcp",
      "memory_search",
    ],
  },

  /*
   * Explicit quota-consuming mode.
   *
   * Codex is NEVER selected automatically.
   * Subagents remain constrained separately by pi-subagents'
   * local-only modelScope.
   */
  codex: {
    provider: "openai-codex",
    model: "gpt-5.6-sol",
    thinking: "low",
    tools: [
      "read",
      "grep",
      "find",
      "ls",
      "bash",
      "edit",
      "write",
      "lsp_diagnostics",
      "subagent",
      "mcp",
      "memory_search",
    ],
  },
};

export default function lardonProfiles(pi: ExtensionAPI) {
  let activeProfile: ProfileName = "local";

  function validTools(requested: string[], ctx: ExtensionContext): string[] {
    const available = new Set(pi.getAllTools().map((tool) => tool.name));

    const valid = requested.filter((name) => available.has(name));
    const missing = requested.filter((name) => !available.has(name));

    if (missing.length > 0) {
      ctx.ui.notify(
        `Lardon profile: unavailable tools ignored: ${missing.join(", ")}`,
        "warning",
      );
    }

    return valid;
  }

  async function applyProfile(
    name: ProfileName,
    ctx: ExtensionContext,
    notify = true,
  ): Promise<boolean> {
    const profile = profiles[name];

    const model = ctx.modelRegistry.find(
      profile.provider,
      profile.model,
    );

    if (!model) {
      ctx.ui.notify(
        `Lardon profile "${name}": model not found: ${profile.provider}/${profile.model}`,
        "error",
      );
      return false;
    }

    const switched = await pi.setModel(model);

    if (!switched) {
      ctx.ui.notify(
        `Lardon profile "${name}": model authentication unavailable`,
        "error",
      );
      return false;
    }

    pi.setThinkingLevel(profile.thinking);

    const tools = validTools(profile.tools, ctx);
    pi.setActiveTools(tools);

    activeProfile = name;

    ctx.ui.setStatus(
      "lardon-profile",
      `Lardon:${name}`,
    );

    if (notify) {
      ctx.ui.notify(
        `Profile ${name} → ${profile.provider}/${profile.model} · thinking ${profile.thinking} · ${tools.length} tools`,
        name === "codex" ? "warning" : "info",
      );
    }

    return true;
  }

  pi.registerCommand("local", {
    description: "Qwen local read-only — default Lardon3D profile",
    handler: async (_args, ctx) => {
      await applyProfile("local", ctx);
    },
  });

  pi.registerCommand("ctx", {
    description: "Qwen local + compact context-mode MCP proxy",
    handler: async (_args, ctx) => {
      await applyProfile("ctx", ctx);
    },
  });

  pi.registerCommand("mem", {
    description: "Qwen local + Hermes memory search",
    handler: async (_args, ctx) => {
      await applyProfile("mem", ctx);
    },
  });

  pi.registerCommand("full", {
    description: "Qwen local + context-mode + memory search",
    handler: async (_args, ctx) => {
      await applyProfile("full", ctx);
    },
  });

  pi.registerCommand("codex", {
    description: "Explicitly switch to Codex Sol-low and enable coding tools",
    handler: async (_args, ctx) => {
      if (ctx.mode === "tui") {
        const confirmed = await ctx.ui.confirm(
          "Use Codex quota?",
          "Switch this session to openai-codex/gpt-5.6-sol (thinking: low)?",
        );

        if (!confirmed) {
          ctx.ui.notify("Codex switch cancelled", "info");
          return;
        }
      }

      await applyProfile("codex", ctx);
    },
  });

  pi.registerCommand("profile", {
    description: "Show current Lardon3D profile and active tools",
    handler: async (_args, ctx) => {
      const profile = profiles[activeProfile];

      ctx.ui.notify(
        [
          `Profile: ${activeProfile}`,
          `Model: ${profile.provider}/${profile.model}`,
          `Thinking: ${profile.thinking}`,
          `Tools: ${pi.getActiveTools().join(", ")}`,
        ].join("\n"),
        "info",
      );
    },
  });

  pi.registerCommand("review", {
    description: "Qwen local + subagents de revue/reconnaissance",
    handler: async (_args, ctx) => {
      await applyProfile("review", ctx);
    },
  });

  /*
   * Fail closed.
   *
   * Every newly started Pi process begins LOCAL even if the previous
   * session ended while Codex was active.
   *
   * Codex can only be entered again with an explicit /codex.
   */
    /*
   * Other extensions may register/reactivate their own tools after
   * session_start (for example mcp and subagent_supervisor).
   *
   * Re-assert the selected Lardon profile immediately before every
   * user-triggered agent run so the model sees exactly the intended
   * tool allowlist.
   */
  pi.on("before_agent_start", async (_event, ctx) => {
    const profile = profiles[activeProfile];

    const available = new Set(
      pi.getAllTools().map((tool) => tool.name),
    );

    const tools = profile.tools.filter(
      (name) => available.has(name),
    );

    pi.setActiveTools(tools);

    ctx.ui.setStatus(
      "lardon-profile",
      `Lardon:${activeProfile}`,
    );
  });
  pi.on("session_start", async (_event, ctx) => {
    await applyProfile("local", ctx, false);
  });
}

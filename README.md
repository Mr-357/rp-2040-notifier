# RP2040 LLM Notifier

A small USB notifier for coding agents. The host executable auto-detects the RP2040 over USB serial and sends one of:

- `ready`
- `done`
- `attention`
- `error`

## Repository layout

```text
.
├── notifier-host-executable
    └── agent-notify.go
└── firmware-notifier/
    └── ...
```

The RP2040 firmware lives in `firmware-notifier`

## Build the host executable

Requires Go.

### Windows

```powershell
go mod tidy
New-Item -ItemType Directory -Force dist | Out-Null
go build -trimpath -ldflags="-s -w" -o agent-notify.exe main.go
```

### Linux

```bash
go mod tidy
mkdir -p dist
go build -trimpath -ldflags="-s -w" -o agent-notify main.go
```

Put the resulting executable somewhere in `PATH`, then test it with:

```bash
agent-notify done
```

## Build and flash the firmware if updated

The easiest setup is the **Raspberry Pi Pico VS Code extension**.

1. Open the `firmware-notifier` in VS Code.
2. Select an RP2040/Pico target and build the project with the Pico SDK extension.
3. Put the board into BOOTSEL mode (hold **BOOT** while resetting it).
4. The Pico appears as a USB drive.
5. Drag the generated `.uf2` file onto that drive.

The board reboots automatically into the new firmware.

## Claude Code

Add hooks to `~/.claude/settings.json`:

```json
{
  "hooks": {
    "UserPromptSubmit": [
      {
        "hooks": [
          { "type": "command", "command": "agent-notify ready" }
        ]
      }
    ],
    "PermissionRequest": [
      {
        "hooks": [
          { "type": "command", "command": "agent-notify attention" }
        ]
      }
    ],
    "PreToolUse": [
      {
        "matcher": "AskUserQuestion",
        "hooks": [
          { "type": "command", "command": "agent-notify attention" }
        ]
      }
    ],
    "PostToolUse": [
      {
        "matcher": "AskUserQuestion",
        "hooks": [
          { "type": "command", "command": "agent-notify ready" }
        ]
      }
    ],
    "Stop": [
      {
        "hooks": [
          { "type": "command", "command": "agent-notify done" }
        ]
      }
    ],
    "StopFailure": [
      {
        "hooks": [
          { "type": "command", "command": "agent-notify error" }
        ]
      }
    ]
  }
}
```

`AskUserQuestion` switches the notifier to `attention` while Claude waits for an answer, then back to `ready` after the answer is submitted.

## OpenCode

Create a global plugin at:

```text
~/.config/opencode/plugins/llm-notifier.js
```

```javascript
export const LLMNotifier = async ({ $ }) => {
  const notify = async (action) => {
    try {
      await $`agent-notify ${action}`
    } catch {
      // The physical notifier must never interfere with OpenCode.
    }
  }

  return {
    event: async ({ event }) => {
      switch (event.type) {
        case "session.created":
        case "permission.replied":
        case "question.replied":
        case "question.rejected":
          await notify("ready")
          break

        case "session.idle":
          await notify("done")
          break

        case "permission.asked":
        case "question.asked":
          await notify("attention")
          break

        case "session.error":
          await notify("error")
          break
      }
    },
  }
}
```

OpenCode question prompts switch the notifier to `attention` while waiting for input, then back to `ready` after the question is answered or dismissed.

Restart OpenCode after adding the plugin.

## Codex

Add this to the user-level `~/.codex/config.toml`:

```toml
notify = ["agent-notify", "done"]
```

Codex appends its notification JSON payload as an additional argument; `agent-notify` ignores additional arguments, so the configured `done` action remains the command sent to the RP2040.

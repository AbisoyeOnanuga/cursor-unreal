# Security

## Secrets

- API keys go through `ISecretStore` (DPAPI on Windows). They are not written to project ini files as plaintext.
- The Cursor bridge receives the key on stdin during init, not as a process argument.

## Tool risk

| Risk | Behaviour |
|------|-----------|
| ReadOnly | Always runs |
| Mutating | Wrapped in an editor undo transaction |
| Dangerous | Approval gate (policy: ask / allow session / allow all) |

Composite tools declare sub-calls so approval covers the whole group.

## Content boundaries

- Write tools refuse `/Engine`, `/Script`, and `/Temp` package paths.
- `source_write` is limited to project/plugin `Source/` trees and refuses this plugin's own sources.
- MCP listens on loopback with a bearer token and Origin check.

## Python / console

`python_exec` and `console_command` are Dangerous. Prefer dedicated pack tools. Skills tell the agent to say when they fall back to Python.

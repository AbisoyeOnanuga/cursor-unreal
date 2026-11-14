# Cursor Unreal bridge

Node sidecar that lets the in-editor **Cursor** provider run agents through [`@cursor/sdk`](https://www.npmjs.com/package/@cursor/sdk) with your Cursor API key.

## Setup

1. Install [Node.js](https://nodejs.org/) **22.13** or newer.
2. In this folder run:

```bash
npm install
```

3. In the Unreal Setup tab, save a **Cursor API key** and set **Provider** to **Cursor**.
4. Pick a model from the chat model list (fetched from your Cursor account), or leave **Cursor Model** as `default` (shown as Auto), which lets Cursor choose. The list is fetched when the key is saved; **Refresh the list** in the Model menu fetches it again.

The editor starts `bridge.mjs` itself. You do not run this folder by hand for normal use.

## How it talks to the editor

Newline-delimited JSON on stdin (editor → bridge) and stdout (bridge → editor). The API key is sent in the `init` message on stdin — never on the command line and never read from the environment by this process.

See `lib/bridge-core.mjs` for the message types.

## Tests

```bash
npm test
```

Uses a stand-in SDK (`test/helpers/fake-sdk.mjs`) so nothing is billed.

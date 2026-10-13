# IPC bridge

The bridge connects the C++ host and the page's JavaScript (including React) over
the platform's native message channel. It is asynchronous, JSON-typed and
Promise-based.

## The JavaScript API — `window.wvc`

Installed at document-start by a ~2 KB bootstrap script, before any app code
runs.

| Member | Signature | Meaning |
| ------ | --------- | ------- |
| `invoke` / `call` | `(channel, payload?) => Promise<T>` | Call a native handler and await its result. |
| `send` | `(channel, payload?) => void` | Fire-and-forget notification to native. |
| `on` | `(channel, cb) => () => void` | Subscribe to a native event; returns an unsubscribe function. |
| `once` | `(channel, cb) => () => void` | One-shot subscription. |
| `off` | `(channel, cb) => void` | Remove a listener. |
| `platform` | `string` | `"windows"`, `"macos"` or `"linux"`. |
| `version` | `string` | Framework version. |
| `isNative` | `boolean` | Always `true` in the app; lets shared web code feature-detect. |

```ts
const info = await window.wvc.invoke<SystemInfo>('system.info');
const off = window.wvc.on('native:greeted', ({ name }) => console.log(name));
window.wvc.send('ui.fileSelected', { path: '/tmp/a.txt' });
```

## The C++ API

```cpp
// Register a handler. Return a Json value -> the JS Promise resolves.
// Throw std::exception        -> the JS Promise rejects with that message.
win.handle("app.greet", [](const Json& args) -> Json {
    return Json{{"message", "Hello " + args["name"].string_or("world")}};
});

// Push an event to the page. Safe from any thread.
win.emit("native:greeted", Json{{"name", "hedy"}, {"at", 1}});

// Run arbitrary JS.
win.eval("console.log('from C++')");
```

Handlers registered with `handle` receive the *payload* only. Handlers may be
registered before `run()`; they are stored on the window's bridge and become
live as soon as the document loads.

## Wire protocol

Every message is one flat JSON object.

```jsonc
// page -> native : invoke
{ "id": 7, "kind": "invoke", "channel": "fs.readText", "payload": { "path": "a.txt" } }

// page -> native : fire-and-forget and handshake
{ "kind": "event", "channel": "ui.fileSelected", "payload": { "path": "..." } }
{ "kind": "ready" }

// native -> page : results and pushes
{ "id": 7, "kind": "resolve", "payload": { "text": "..." } }
{ "id": 7, "kind": "reject",  "error": "cannot open a.txt" }
{ "kind": "event", "channel": "tick", "payload": { "at": 1 } }
```

`id` values are allocated by the page; native echoes them back verbatim. The
payload may be any JSON value. Native replies are delivered by evaluating
`window.__wvc.__dispatch(<message>)`.

### Handshake

The page posts `{kind:"ready"}` once the bootstrap has installed `window.wvc`.
Native queues outgoing events until then and flushes them in order, so no event
is lost to a race between "document loaded" and "React mounted its listeners"
(provided the listener is registered before the event is emitted; `on()`
returns an unsubscribe handle for cleanup).

### `send` vs `invoke`

* `send` is a one-way notification; native's `on_event_hook` fires and nothing
  is returned.
* `invoke` allocates an `id` and expects exactly one `resolve` or `reject`.

## Channel naming

Use dotted, namespaced channels. The bundled services own these namespaces:

| Namespace | Provided by |
| --------- | ----------- |
| `system.*`, `shell.*` | `Window::install_system_services()` |
| `fs.*` | `Window::install_fs_services([sandbox_root])` |
| `dialog.*` | `Window::install_dialog_services()` |
| `clipboard.*` | `Window::install_clipboard_services()` |

`Window::install_default_services()` installs all of them. Applications should
use their own prefix (e.g. `app.*`) for custom channels.

## Built-in channels

### `system.*`, `shell.*`

| Channel | Payload | Result |
| ------- | ------- | ------ |
| `system.info` | — | framework, version, platform, family, arch, cpus, pid, cwd, home, temp, appData, configDir |
| `system.platform` | — | `{ platform, family }` |
| `system.env` | `{ key }` | `{ value }` |
| `shell.open` | `{ target }` | `{ ok }` — opens a URL/file with the OS handler |
| `shell.showItemInFolder` | `{ path }` | `{ ok }` |

### `fs.*`

| Channel | Payload | Result |
| ------- | ------- | ------ |
| `fs.readText` | `{ path }` | `{ text, path }` |
| `fs.writeText` | `{ path, text }` | `{ path, bytes }` |
| `fs.readBinary` | `{ path }` | `{ data (base64), encoding, bytes }` |
| `fs.writeBinary` | `{ path, data (base64) }` | `{ path, bytes }` |
| `fs.exists` | `{ path }` | `{ exists, isFile, isDirectory }` |
| `fs.stat` | `{ path }` | `{ path, size, isDirectory }` |
| `fs.list` | `{ path }` | `{ path, entries: [{ name, path, isDirectory, size }] }` |
| `fs.mkdir` | `{ path, recursive? }` | `{ path }` |
| `fs.remove` | `{ path, recursive? }` | `{ ok }` |
| `fs.join` | `{ parts: [...] }` | `{ path }` |

### `dialog.*`

| Channel | Payload | Result |
| ------- | ------- | ------ |
| `dialog.open` | `{ title?, defaultPath?, multiple?, directory?, filters?: [{name, extensions}] }` | `{ paths, path, canceled }` |
| `dialog.save` | `{ title?, defaultPath?, defaultName?, filters? }` | `{ path, canceled }` |
| `dialog.message` | `{ title?, message, type?, buttons? }` | `{ response }` (0 = Ok/Yes, 1 = Cancel/No) |

### `clipboard.*`

| Channel | Payload | Result |
| ------- | ------- | ------ |
| `clipboard.readText` | — | `{ text }` |
| `clipboard.writeText` | `{ text }` | `{ ok }` |

## Security

The bridge is only reachable from within the app's own web view — there is no
listening socket and nothing is exposed to the network. Recommendations:

* **Never expose raw paths.** Prefer `dialog.open`, which returns an
  OS-vetted path, over letting the page pass arbitrary paths to `fs.*`.
* **Sandbox the filesystem.** `install_fs_services(root)` confines every `fs.*`
  call to `root`; traversal outside it is rejected:

  ```cpp
  window.install_fs_services("/home/me/AppData");   // fs.* cannot escape this
  ```

* **Validate payloads.** Treat every `Json` field as untrusted; use
  `int_or`, `string_or`, `bool_or` and reject unexpected shapes.
* **Throw to reject.** Handlers that throw turn into rejected Promises without
  crashing the host.
* Keep the page offline-capable: it is served from `app://`, so it does not need
  network access unless you give it one.

## React example

```tsx
import { useEffect, useState } from 'react';

interface SystemInfo { platform: string; cpus: number }

export default function App() {
  const [info, setInfo] = useState<SystemInfo | null>(null);
  const [count, setCount] = useState(0);

  useEffect(() => {
    window.wvc.invoke<SystemInfo>('system.info').then(setInfo);
    return window.wvc.on<{ at: number }>('native:tick', (e) => console.log('tick', e.at));
  }, []);

  async function save() {
    const { path } = await window.wvc.invoke<{ path: string }>('dialog.save', {
      defaultName: 'notes.txt',
      filters: [{ name: 'Text', extensions: ['txt'] }],
    });
    if (path) await window.wvc.invoke('fs.writeText', { path, text: `count=${count}` });
  }

  return (
    <div>
      <p>{info?.platform} · {info?.cpus} cpus</p>
      <button onClick={() => setCount((c) => c + 1)}>{count}</button>
      <button onClick={save}>Save</button>
    </div>
  );
}
```

import { useEffect, useState } from 'react';

interface SystemInfo {
  platform: string;
  family: string;
  arch: string;
  cpus: number;
  home: string;
  appData: string;
  frameworkVersion: string;
}

interface FileResult {
  canceled: boolean;
  path?: string;
  content?: string;
}

type Tab = 'system' | 'file' | 'file-system';

export default function App() {
  const [count, setCount] = useState(0);
  const [info, setInfo] = useState<SystemInfo | null>(null);
  const [file, setFile] = useState<FileResult | null>(null);
  const [directory, setDirectory] = useState<string>('');
  const [clipboard, setClipboard] = useState('');
  const [tab, setTab] = useState<Tab>('system');
  const [events, setEvents] = useState<string[]>([]);

  useEffect(() => {
    window.wvc.invoke<SystemInfo>('system.info').then((data) => {
      setInfo(data);
      console.log(`[react-counter] bridge ready · ${data.platform}/${data.family} · ${data.cpus} cpus`);
    });

    const offReady = window.wvc.on('native:ready', (payload: { version: string }) => {
      setEvents((e) => [...e, `native:ready  v${payload.version}`]);
    });
    const offLoaded = window.wvc.on('native:fileLoaded', (payload: { path: string }) => {
      setEvents((e) => [...e, `native:fileLoaded  ${payload.path}`]);
    });
    return () => {
      offReady();
      offLoaded();
    };
  }, []);

  async function openFile() {
    const result = await window.wvc.invoke<FileResult>('app.readFileViaDialog');
    setFile(result);
    if (!result.canceled && result.path) {
      window.wvc.send('ui.fileSelected', { path: result.path });
    }
  }

  async function listDirectory() {
    const result = await window.wvc.invoke<{ entries: { name: string; isDirectory: boolean }[] }>(
      'fs.list',
      { path: info?.home ?? '.' }
    );
    setDirectory(result.entries.map((e) => `${e.isDirectory ? '📁' : '📄'} ${e.name}`).join('\n'));
  }

  async function copyToClipboard() {
    await window.wvc.invoke('clipboard.writeText', { text: `Counter is at ${count}` });
    const back = await window.wvc.invoke<{ text: string }>('clipboard.readText');
    setClipboard(back.text);
  }

  return (
    <main className="app">
      <header>
        <h1>React Counter</h1>
        <span className="badge">WebViewCpp · {window.wvc.platform}</span>
      </header>

      <section className="row">
        <button onClick={() => setCount((c) => c + 1)}>+1</button>
        <button onClick={() => setCount((c) => c - 1)}>−1</button>
        <span className="counter">{count}</span>
      </section>

      <nav className="tabs">
        {(['system', 'file', 'file-system'] as Tab[]).map((t) => (
          <button key={t} className={t === tab ? 'active' : ''} onClick={() => setTab(t)}>
            {t}
          </button>
        ))}
      </nav>

      {tab === 'system' && info && (
        <section className="card">
          <h2>Native system.info</h2>
          <dl className="kv">
            {Object.entries(info).map(([k, v]) => (
              <div key={k}>
                <dt>{k}</dt>
                <dd>{String(v)}</dd>
              </div>
            ))}
          </dl>
          <button onClick={copyToClipboard}>Copy counter to clipboard</button>
          {clipboard && <p className="result">clipboard → “{clipboard}”</p>}
        </section>
      )}

      {tab === 'file' && (
        <section className="card">
          <h2>Native file dialog + read</h2>
          <button onClick={openFile}>Open file…</button>
          {file?.canceled && <p className="dim">cancelled</p>}
          {file && !file.canceled && (
            <>
              <p className="result">{file.path}</p>
              <pre>{file.content}</pre>
            </>
          )}
        </section>
      )}

      {tab === 'file-system' && (
        <section className="card">
          <h2>fs.list in your home directory</h2>
          <button onClick={listDirectory}>List {info?.home}</button>
          <pre>{directory}</pre>
        </section>
      )}

      <section className="card">
        <h2>Native events (C++ → JS)</h2>
        {events.length === 0 ? <p className="dim">waiting…</p> : events.map((e, i) => <p key={i}>{e}</p>)}
        <button onClick={() => window.wvc.invoke('app.newWindow', { title: 'Second counter' })}>
          Open a second native window
        </button>
      </section>
    </main>
  );
}

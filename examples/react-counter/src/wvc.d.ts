// Ambient typing for the WebViewCpp bridge (`window.wvc`).
export {};

declare global {
  interface WvcBridge {
    invoke<T = unknown>(channel: string, payload?: unknown): Promise<T>;
    call<T = unknown>(channel: string, payload?: unknown): Promise<T>;
    send(channel: string, payload?: unknown): void;
    on(channel: string, cb: (payload: any) => void): () => void;
    off(channel: string, cb: (payload: any) => void): void;
    once(channel: string, cb: (payload: any) => void): () => void;
    platform: string;
    version: string;
    isNative: boolean;
  }
  interface Window { wvc: WvcBridge; __wvc: WvcBridge; }
}

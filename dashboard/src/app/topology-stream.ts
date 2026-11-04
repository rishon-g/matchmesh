import { Injectable, InjectionToken, inject, signal } from '@angular/core';
import { toSignal } from '@angular/core/rxjs-interop';
import { defer, repeat, retry, timer } from 'rxjs';
import { webSocket } from 'rxjs/webSocket';

import { ClusterFeed, ConnectionState } from './cluster-feed';
import { Snapshot } from './topology';

/** Where the gateway's WebSocket is: the page's own host, at /ws (nginx or the dev server proxies it). */
export const GATEWAY_SOCKET_URL = new InjectionToken<string>('GATEWAY_SOCKET_URL', {
  providedIn: 'root',
  factory: () => `${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/ws`,
});

/** Waits before reconnect attempt `n` (1, 2, ...): 0.5 s, 1 s, 2 s, then every 5 s. */
export function reconnectDelay(attempt: number): number {
  return Math.min(500 * 2 ** (attempt - 1), 5000);
}

/**
 * The live cluster snapshot, as a signal. Subscribes to the gateway's
 * WebSocket with RxJS and reconnects on its own (with backoff) if the
 * gateway restarts or the connection drops.
 */
@Injectable({ providedIn: 'root' })
export class TopologyStream extends ClusterFeed {
  private readonly url = inject(GATEWAY_SOCKET_URL);

  override readonly state = signal<ConnectionState>('connecting');

  override readonly snapshot = toSignal<Snapshot | null>(
    defer(() =>
      webSocket<Snapshot>({
        url: this.url,
        openObserver: { next: () => this.state.set('live') },
        closeObserver: { next: () => this.state.set('reconnecting') },
      }),
    ).pipe(
      // An error (the gateway is unreachable) and a clean close both lead to
      // another attempt; a successful connection resets the backoff.
      retry({ delay: (_error, attempt) => timer(reconnectDelay(attempt)), resetOnSuccess: true }),
      repeat({ delay: () => timer(reconnectDelay(1)) }),
    ),
    { initialValue: null },
  );
}

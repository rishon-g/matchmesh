import { InjectionToken, Signal } from '@angular/core';

import { environment } from '../environments/environment';

import { Snapshot } from './topology';

export type ConnectionState = 'connecting' | 'live' | 'reconnecting' | 'simulated' | 'failed';

/**
 * Where the page's snapshots come from: the gateway's WebSocket
 * (TopologyStream) or the cluster simulated in the browser (SimulatedCluster).
 * The components only ever see this.
 */
export abstract class ClusterFeed {
  abstract readonly state: Signal<ConnectionState>;
  abstract readonly snapshot: Signal<Snapshot | null>;
}

/** True in the GitHub Pages build, which simulates the cluster instead of connecting to one. */
export const DEMO_MODE = new InjectionToken<boolean>('DEMO_MODE', {
  providedIn: 'root',
  factory: () => environment.demo,
});

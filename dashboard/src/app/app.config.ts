import { ApplicationConfig, inject, provideBrowserGlobalErrorListeners } from '@angular/core';

import { ClusterFeed, DEMO_MODE } from './cluster-feed';
import { SimulatedCluster } from './simulated-cluster';
import { TopologyStream } from './topology-stream';

export const appConfig: ApplicationConfig = {
  providers: [
    provideBrowserGlobalErrorListeners(),
    // A real cluster through the gateway, or (the GitHub Pages build) one
    // simulated in the browser. Only the chosen one is ever created.
    {
      provide: ClusterFeed,
      useFactory: () => (inject(DEMO_MODE) ? inject(SimulatedCluster) : inject(TopologyStream)),
    },
  ],
};

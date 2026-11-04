import { ChangeDetectionStrategy, Component, computed, inject } from '@angular/core';

import { ClusterFeed, DEMO_MODE } from './cluster-feed';
import { DemoPanel } from './demo-panel';
import { LobbyFeed } from './lobby-feed';
import { NodeList } from './node-list';
import { RingChart } from './ring-chart';

@Component({
  selector: 'app-root',
  imports: [RingChart, NodeList, LobbyFeed, DemoPanel],
  changeDetection: ChangeDetectionStrategy.OnPush,
  templateUrl: './app.html',
  styleUrl: './app.css',
})
export class App {
  protected readonly stream = inject(ClusterFeed);
  protected readonly snapshot = this.stream.snapshot;

  protected readonly nodeIds = computed(() => this.snapshot()?.nodes.map((node) => node.id) ?? []);

  protected readonly serving = computed(
    () => this.snapshot()?.nodes.filter((node) => node.reachable && node.member).length ?? 0,
  );

  /** The page runs its own simulated cluster rather than following a real one. */
  protected readonly demo = inject(DEMO_MODE);

  protected readonly live = computed(() => ['live', 'simulated'].includes(this.stream.state()));

  protected readonly connection = computed(() => {
    switch (this.stream.state()) {
      case 'live':
        return 'Live';
      case 'connecting':
        return 'Connecting to the gateway';
      case 'reconnecting':
        return 'Gateway unreachable, reconnecting';
      case 'simulated':
        return 'Simulated in your browser';
      case 'failed':
        return 'The simulator could not start';
    }
  });
}

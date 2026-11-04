import { ChangeDetectionStrategy, Component, computed, inject } from '@angular/core';

import { LobbyFeed } from './lobby-feed';
import { NodeList } from './node-list';
import { RingChart } from './ring-chart';
import { TopologyStream } from './topology-stream';

@Component({
  selector: 'app-root',
  imports: [RingChart, NodeList, LobbyFeed],
  changeDetection: ChangeDetectionStrategy.OnPush,
  templateUrl: './app.html',
  styleUrl: './app.css',
})
export class App {
  protected readonly stream = inject(TopologyStream);
  protected readonly snapshot = this.stream.snapshot;

  protected readonly nodeIds = computed(() => this.snapshot()?.nodes.map((node) => node.id) ?? []);

  protected readonly serving = computed(
    () => this.snapshot()?.nodes.filter((node) => node.reachable && node.member).length ?? 0,
  );

  protected readonly connection = computed(() => {
    switch (this.stream.state()) {
      case 'live':
        return 'Live';
      case 'connecting':
        return 'Connecting to the gateway';
      case 'reconnecting':
        return 'Gateway unreachable, reconnecting';
    }
  });
}

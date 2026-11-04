import { ChangeDetectionStrategy, Component, computed, input } from '@angular/core';

import { colourFor } from './ring-geometry';
import { ClusterNode } from './topology';

/** Every configured node: whether it's serving, and how much it holds. */
@Component({
  selector: 'mm-node-list',
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <h2>Nodes</h2>
    <table>
      <thead>
        <tr>
          <th scope="col">Node</th>
          <th scope="col">State</th>
          <th scope="col" class="number">Queues</th>
          <th scope="col" class="number">Waiting</th>
        </tr>
      </thead>
      <tbody>
        @for (node of rows(); track node.id) {
          <tr [class.down]="node.state !== 'Serving'">
            <th scope="row">
              <span class="swatch" [style.background]="node.colour"></span>{{ node.id }}
            </th>
            <td>{{ node.state }}</td>
            <td class="number">{{ node.queues }}</td>
            <td class="number">{{ node.queued }}</td>
          </tr>
        }
      </tbody>
    </table>
  `,
  styleUrl: './node-list.css',
})
export class NodeList {
  readonly nodes = input.required<ClusterNode[]>();

  protected readonly rows = computed(() => {
    const ids = this.nodes().map((node) => node.id);
    return this.nodes().map((node) => ({
      ...node,
      colour: colourFor(ids, node.id),
      state: !node.reachable ? 'Unreachable' : node.member ? 'Serving' : 'Rejoining',
    }));
  });
}

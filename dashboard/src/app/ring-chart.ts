import { ChangeDetectionStrategy, Component, computed, input } from '@angular/core';

import { bandPath, colourFor, dotRadius, ownership, polar } from './ring-geometry';
import { Snapshot } from './topology';

const OUTER = 182; // the ownership band's outer and inner radius
const INNER = 160;
const DOTS = 136; // queues sit just inside the band

/**
 * The consistent hash ring, drawn from the real data: the coloured band
 * shows which node owns each stretch (every node holds 100 places round the
 * ring), and each dot is a queue at the position it hashes to, filled with
 * its primary's colour and ringed with its backup's.
 */
@Component({
  selector: 'mm-ring-chart',
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <svg viewBox="-200 -200 400 400" role="img" [attr.aria-label]="summary()">
      <circle class="track" r="171" />
      @for (segment of segments(); track $index) {
        <path [attr.d]="segment.d" [attr.fill]="segment.colour" [attr.stroke]="segment.colour" />
      }
      @for (dot of dots(); track dot.queue) {
        <circle
          class="queue"
          [attr.cx]="dot.x"
          [attr.cy]="dot.y"
          [attr.r]="dot.r"
          [attr.fill]="dot.fill"
          [attr.stroke]="dot.stroke"
        >
          <title>{{ dot.label }}</title>
        </circle>
      }
      <text class="epoch-label" y="-48">epoch</text>
      <text class="epoch" y="26">{{ snapshot().epoch }}</text>
      <text class="coordinator" y="56">coordinated by {{ snapshot().coordinator || 'nobody' }}</text>
    </svg>
  `,
  styleUrl: './ring-chart.css',
})
export class RingChart {
  readonly snapshot = input.required<Snapshot>();

  private readonly nodeIds = computed(() => this.snapshot().nodes.map((node) => node.id));

  protected readonly segments = computed(() =>
    ownership(this.snapshot().ring).map((segment) => ({
      d: bandPath(segment.from, segment.to, INNER, OUTER),
      colour: colourFor(this.nodeIds(), segment.node),
    })),
  );

  protected readonly dots = computed(() => {
    const shards = this.snapshot().shards;
    const most = Math.max(0, ...shards.map((shard) => shard.queued));
    return shards.map((shard) => {
      const { x, y } = polar(shard.at, DOTS);
      return {
        queue: shard.queue,
        x: x.toFixed(2),
        y: y.toFixed(2),
        r: dotRadius(shard.queued, most).toFixed(2),
        fill: colourFor(this.nodeIds(), shard.primary),
        stroke: shard.backup ? colourFor(this.nodeIds(), shard.backup) : 'none',
        label:
          `${shard.queue}: ${shard.queued} waiting, on ${shard.primary}` +
          (shard.backup ? `, copied to ${shard.backup}` : ''),
      };
    });
  });

  protected readonly summary = computed(() => {
    const s = this.snapshot();
    return `Hash ring at epoch ${s.epoch}: ${s.members} nodes own ${s.shards.length} queues.`;
  });
}

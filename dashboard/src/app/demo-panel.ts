import { ChangeDetectionStrategy, Component, computed, inject, input } from '@angular/core';

import { colourFor } from './ring-geometry';
import { SimulatedCluster } from './simulated-cluster';
import { SimEvent, Snapshot } from './topology';

/** How many of the newest events the log shows. */
export const EVENT_LOG_LENGTH = 9;

const EVENT_TAGS: Record<SimEvent['kind'], string> = {
  action: 'you',
  view: 'view',
  detect: 'detect',
  handover: 'hand-over',
  promote: 'take-over',
  warn: 'invariant',
};

/** "4.2 s" since the simulation started. */
export function elapsed(event: SimEvent, nowMs: number, elapsedMs: number): string {
  return `${((event.at - (nowMs - elapsedMs)) / 1000).toFixed(1)} s`;
}

/**
 * The demo's controls (kill, restart, split, traffic), the invariants the
 * simulator checks live, and what the cluster just did about it.
 */
@Component({
  selector: 'mm-demo-panel',
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <div class="intro">
      <h2>Break it yourself</h2>
      <p>
        This page runs MatchMesh's real C++ core, compiled to WebAssembly: five nodes, each with its
        own failure detector, matchmaking engine and replication, talking over a simulated network.
        Players keep arriving and retrying like real clients.
      </p>
      <ul class="try">
        <li>
          <b>Kill one node.</b> About 1.5 s later its queues move to their backups. Nobody is lost.
        </li>
        <li>
          <b>Kill two neighbours.</b> A queue whose primary and backup were both on them loses its
          players.
        </li>
        <li>
          <b>Split the network.</b> Both halves serve the same queues (split brain) until you heal
          it.
        </li>
      </ul>
    </div>

    <div class="controls">
      <h3>Nodes</h3>
      <ul class="node-buttons">
        @for (node of nodes(); track node.id) {
          <li [class.down]="!node.up">
            <span class="swatch" [style.background]="node.colour"></span>
            <span class="id">{{ node.id }}</span>
            <button
              type="button"
              (click)="toggle(node.id, node.up)"
              [attr.aria-label]="(node.up ? 'Kill ' : 'Restart ') + node.id"
            >
              {{ node.up ? 'Kill' : 'Restart' }}
            </button>
          </li>
        }
      </ul>

      <h3>Network</h3>
      <button type="button" class="wide" (click)="split() ? sim.heal() : sim.split()">
        {{ split() ? 'Heal the network' : 'Split the network' }}
      </button>
      <p class="note">
        {{
          split()
            ? 'node-a, node-b cannot reach node-c, node-d, node-e'
            : 'Splits node-a, node-b from the other three'
        }}
      </p>

      <h3><label for="rate">Players arriving</label></h3>
      <div class="rate">
        <input
          id="rate"
          type="range"
          min="0"
          max="100"
          step="5"
          [value]="sim.rate()"
          (input)="setRate($event)"
        />
        <output for="rate">{{ sim.rate() }}/s</output>
      </div>

      <button type="button" class="reset" (click)="sim.reset()">Start over</button>
    </div>

    <div class="checks">
      <h3>Checked live, every 250 ms</h3>
      @if (checks(); as c) {
        <dl>
          <div>
            <dt>Told “you’re in”</dt>
            <dd>{{ c.acknowledged }}</dd>
          </div>
          <div>
            <dt>In a lobby</dt>
            <dd>{{ c.in_lobby }}</dd>
          </div>
          <div>
            <dt>Still waiting</dt>
            <dd>{{ c.queued }}</dd>
          </div>
          <div [class.bad]="c.lost > 0">
            <dt>Lost</dt>
            <dd>{{ c.lost }}</dd>
          </div>
          <div [class.bad]="c.in_two_lobbies > 0">
            <dt>In two lobbies</dt>
            <dd>{{ c.in_two_lobbies }}</dd>
          </div>
          <div [class.bad]="c.contested_queues > 0">
            <dt>Queues served twice</dt>
            <dd>{{ c.contested_queues }}</dd>
          </div>
        </dl>
        <p class="note">{{ c.retries }} refused attempts were retried by clients.</p>
      }
    </div>

    <div class="events">
      <h3>What the cluster did</h3>
      <ol>
        @for (event of events(); track event.seq) {
          <li [class]="event.kind">
            <span class="when">{{ event.when }}</span>
            <span class="tag">{{ event.tag }}</span>
            <span class="text">{{ event.text }}</span>
          </li>
        } @empty {
          <li class="quiet">Nothing yet.</li>
        }
      </ol>
    </div>
  `,
  styleUrl: './demo-panel.css',
})
export class DemoPanel {
  protected readonly sim = inject(SimulatedCluster);

  readonly snapshot = input.required<Snapshot>();

  protected readonly split = computed(() => this.snapshot().sim?.split ?? false);
  protected readonly checks = computed(() => this.snapshot().sim?.invariants ?? null);

  protected readonly nodes = computed(() => {
    const ids = this.snapshot().nodes.map((node) => node.id);
    return this.snapshot().nodes.map((node) => ({
      id: node.id,
      up: node.reachable,
      colour: colourFor(ids, node.id),
    }));
  });

  protected readonly events = computed(() => {
    const sim = this.snapshot().sim;
    if (!sim) {
      return [];
    }
    return sim.events
      .slice(-EVENT_LOG_LENGTH)
      .reverse()
      .map((event) => ({
        ...event,
        tag: EVENT_TAGS[event.kind],
        when: elapsed(event, sim.now, sim.elapsed_ms),
      }));
  });

  protected toggle(node: string, up: boolean): void {
    if (up) {
      this.sim.kill(node);
    } else {
      this.sim.restart(node);
    }
  }

  protected setRate(event: Event): void {
    this.sim.setRate(Number((event.target as HTMLInputElement).value));
  }
}

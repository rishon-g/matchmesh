import { TestBed } from '@angular/core/testing';

import { App } from './app';
import { ClusterFeed, DEMO_MODE } from './cluster-feed';
import { elapsed, EVENT_LOG_LENGTH } from './demo-panel';
import {
  DEFAULT_RATE,
  FRAME_SCHEDULER,
  SIM_LOADER,
  SimEngine,
  SimModule,
  SimulatedCluster,
  SNAPSHOT_EVERY_MS,
} from './simulated-cluster';
import { SimEvent, Snapshot } from './topology';

const NODES = ['node-a', 'node-b', 'node-c', 'node-d', 'node-e'];

/** Stands in for the WebAssembly sim::Cluster: records calls, reports a tiny cluster. */
class FakeEngine implements SimEngine {
  static created: FakeEngine[] = [];
  readonly calls: string[] = [];
  advanced = 0;
  down = new Set<string>();
  split_ = false;
  deleted = false;

  constructor(
    readonly seed: number,
    readonly start: number,
    readonly rate: number,
  ) {
    FakeEngine.created.push(this);
  }

  advance(ms: number) {
    this.advanced += ms;
  }
  kill(node: string) {
    this.calls.push(`kill ${node}`);
    this.down.add(node);
  }
  restart(node: string) {
    this.calls.push(`restart ${node}`);
    this.down.delete(node);
  }
  split() {
    this.calls.push('split');
    this.split_ = true;
  }
  heal() {
    this.calls.push('heal');
    this.split_ = false;
  }
  setRate(rate: number) {
    this.calls.push(`rate ${rate}`);
  }
  delete() {
    this.deleted = true;
  }
  snapshot(): string {
    const events: SimEvent[] = Array.from({ length: 12 }, (_, i) => ({
      seq: i + 1,
      at: 1_000 + i * 100,
      kind: i === 11 ? 'warn' : 'view',
      text: `event ${i + 1}`,
    }));
    const doc: Snapshot = {
      epoch: 3,
      coordinator: 'node-a',
      members: 5 - this.down.size,
      queued_players: 4,
      nodes: NODES.map((id) => ({
        id,
        address: this.down.has(id) ? 'down' : 'simulated',
        reachable: !this.down.has(id),
        member: !this.down.has(id) && !(this.split_ && id > 'node-b'),
        queues: 1,
        queued: 1,
        epoch: 3,
        side: this.split_ ? (id > 'node-b' ? 2 : 1) : 0,
      })),
      ring: [{ node: 'node-a', at: 0.5 }],
      shards: [
        {
          queue: 'eu/ranked/3',
          region: 'eu',
          mode: 'ranked',
          bracket: 3,
          primary: 'node-a',
          backup: 'node-b',
          queued: 4,
          at: 0.2,
          contested: this.split_,
        },
      ],
      lobbies: [],
      sim: {
        now: 2_500,
        elapsed_ms: 2_000,
        rate: this.rate,
        split: this.split_,
        waiting_clients: 0,
        invariants: {
          acknowledged: 40,
          in_lobby: 30,
          queued: 9,
          lost: 1,
          in_two_lobbies: 0,
          contested_queues: this.split_ ? 1 : 0,
          retries: 3,
        },
        events,
      },
    };
    return JSON.stringify(doc);
  }
}

const fakeModule: SimModule = { Cluster: FakeEngine };

async function start() {
  FakeEngine.created = [];
  const frames: ((now: number) => void)[] = [];
  TestBed.configureTestingModule({
    imports: [App],
    providers: [
      { provide: DEMO_MODE, useValue: true },
      { provide: SIM_LOADER, useValue: () => Promise.resolve(fakeModule) },
      {
        provide: FRAME_SCHEDULER,
        useValue: (callback: (now: number) => void) => frames.push(callback),
      },
      { provide: ClusterFeed, useExisting: SimulatedCluster },
    ],
  });
  const fixture = TestBed.createComponent(App);
  await fixture.whenStable();
  fixture.detectChanges();
  const sim = TestBed.inject(SimulatedCluster);
  const frame = (now: number) => {
    frames.shift()?.(now);
    fixture.detectChanges();
  };
  const click = (label: string) => {
    const button = [...(fixture.nativeElement as HTMLElement).querySelectorAll('button')].find(
      (b) => b.textContent?.trim() === label || b.getAttribute('aria-label') === label,
    );
    if (!button) {
      throw new Error(`no button ${label}`);
    }
    button.click();
    fixture.detectChanges();
  };
  return {
    page: fixture.nativeElement as HTMLElement,
    sim,
    frame,
    click,
    engine: () => FakeEngine.created.at(-1)!,
  };
}

describe('Demo mode', () => {
  it('starts a simulated cluster and shows it with the controls', async () => {
    const { page, engine } = await start();
    expect(engine().rate).toBe(DEFAULT_RATE);
    expect(page.querySelector('.connection')?.textContent).toContain('Simulated in your browser');
    expect(page.querySelector('mm-demo-panel')).not.toBeNull();
    expect(page.querySelectorAll('mm-demo-panel .node-buttons li').length).toBe(5);
  });

  it('moves simulated time with the display, never more than 250 ms a frame', async () => {
    const { frame, engine } = await start();
    frame(1_000); // the first frame only sets the starting point
    frame(1_016);
    frame(9_000); // the tab was hidden for a while
    expect(engine().advanced).toBe(16 + 250);
  });

  it('kills and restarts nodes, and says which are down', async () => {
    const { page, click, engine } = await start();
    click('Kill node-c');
    expect(engine().calls).toEqual(['kill node-c']);
    expect(page.querySelector('mm-node-list')?.textContent).toContain('Unreachable');
    click('Restart node-c');
    expect(engine().calls).toEqual(['kill node-c', 'restart node-c']);
  });

  it('splits and heals the network, marking the queues both halves serve', async () => {
    const { page, click, engine } = await start();
    click('Split the network');
    expect(engine().calls).toEqual(['split']);
    expect(page.querySelector('circle.queue.contested')).not.toBeNull();
    expect(page.querySelector('mm-node-list')?.textContent).toContain('Split off');
    click('Heal the network');
    expect(engine().calls).toEqual(['split', 'heal']);
  });

  it('flags broken invariants and shows the newest events first', async () => {
    const { page } = await start();
    const bad = [...page.querySelectorAll('mm-demo-panel dl .bad dt')].map((dt) => dt.textContent);
    expect(bad).toEqual(['Lost']);
    const events = page.querySelectorAll('mm-demo-panel .events li');
    expect(events.length).toBe(EVENT_LOG_LENGTH);
    expect(events[0].textContent).toContain('event 12');
    expect(events[0].classList).toContain('warn');
  });

  it('starts over with a fresh cluster', async () => {
    const { click } = await start();
    const first = FakeEngine.created[0];
    click('Start over');
    expect(first.deleted).toBe(true);
    expect(FakeEngine.created.length).toBe(2);
  });

  it(`redraws about every ${SNAPSHOT_EVERY_MS} ms, not every frame`, async () => {
    const { sim } = await start();
    const before = sim.snapshot();
    sim.advance(SNAPSHOT_EVERY_MS / 2);
    expect(sim.snapshot()).toBe(before);
    sim.advance(SNAPSHOT_EVERY_MS / 2);
    expect(sim.snapshot()).not.toBe(before);
  });
});

describe('elapsed', () => {
  it('counts from the start of the simulation', () => {
    const event: SimEvent = { seq: 1, at: 1_700, kind: 'view', text: '' };
    expect(elapsed(event, 2_500, 2_000)).toBe('1.2 s');
  });
});

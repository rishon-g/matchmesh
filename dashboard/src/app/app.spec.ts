import { signal } from '@angular/core';
import { TestBed } from '@angular/core/testing';

import { App } from './app';
import { ago, FEED_LENGTH } from './lobby-feed';
import { Snapshot } from './topology';
import { ConnectionState, reconnectDelay, TopologyStream } from './topology-stream';

const snapshot: Snapshot = {
  epoch: 4,
  coordinator: 'node-a',
  members: 2,
  queued_players: 9,
  nodes: [
    { id: 'node-a', address: 'a:1', reachable: true, member: true, queues: 2, queued: 9 },
    { id: 'node-b', address: 'b:1', reachable: true, member: false, queues: 0, queued: 0 },
    { id: 'node-c', address: 'c:1', reachable: false, member: false, queues: 0, queued: 0 },
  ],
  ring: [
    { node: 'node-a', at: 0.25 },
    { node: 'node-b', at: 0.75 },
  ],
  shards: [
    { queue: 'eu/ranked/2', region: 'eu', mode: 'ranked', bracket: 2, primary: 'node-a', backup: 'node-b', queued: 9, at: 0.1 },
  ],
  lobbies: Array.from({ length: 15 }, (_, i) => ({
    id: `L${i}`,
    queue: 'eu/ranked/2',
    node: 'node-a',
    formed_at: Date.now() - i * 1000,
    players: [{ id: `p${i}`, rating: 1300 }],
  })),
};

function render(state: ConnectionState, value: Snapshot | null) {
  TestBed.configureTestingModule({
    imports: [App],
    providers: [{ provide: TopologyStream, useValue: { state: signal(state), snapshot: signal(value) } }],
  });
  const fixture = TestBed.createComponent(App);
  fixture.detectChanges();
  return fixture.nativeElement as HTMLElement;
}

describe('App', () => {
  it('shows the cluster from a snapshot', () => {
    const page = render('live', snapshot);
    expect(page.querySelector('.connection')?.textContent).toContain('Live');
    expect(page.querySelector('.facts')?.textContent).toContain('1 of 3 nodes serving');
    expect(page.querySelector('.epoch')?.textContent?.trim()).toBe('4');
    expect(page.querySelectorAll('svg path').length).toBe(3); // a, b, then a again after wrapping
    expect(page.querySelectorAll('svg circle.queue').length).toBe(1);
  });

  it('says which nodes are not serving and why', () => {
    const states = [...render('live', snapshot).querySelectorAll('mm-node-list tbody td:nth-child(2)')].map(
      (cell) => cell.textContent?.trim(),
    );
    expect(states).toEqual(['Serving', 'Rejoining', 'Unreachable']);
  });

  it(`shows only the newest ${FEED_LENGTH} lobbies`, () => {
    const items = render('live', snapshot).querySelectorAll('mm-lobby-feed li');
    expect(items.length).toBe(FEED_LENGTH);
    expect(items[0].textContent).toContain('p0');
  });

  it('explains what to do before the first update arrives', () => {
    const page = render('reconnecting', null);
    expect(page.querySelector('.connection')?.textContent).toContain('reconnecting');
    expect(page.querySelector('.waiting')?.textContent).toContain('docker compose up');
  });
});

describe('ago', () => {
  it('reads naturally', () => {
    expect(ago(10_000, 10_500)).toBe('just now');
    expect(ago(0, 12_000)).toBe('12 s ago');
    expect(ago(0, 185_000)).toBe('3 min ago');
  });
});

describe('reconnectDelay', () => {
  it('backs off to at most 5 seconds', () => {
    expect([1, 2, 3, 4, 5, 9].map(reconnectDelay)).toEqual([500, 1000, 2000, 4000, 5000, 5000]);
  });
});

import { DestroyRef, Injectable, InjectionToken, inject, signal } from '@angular/core';

import { ClusterFeed, ConnectionState } from './cluster-feed';
import { Snapshot } from './topology';

/** The C++ sim::Cluster, as embind exposes it (see demo/bindings.cpp). */
export interface SimEngine {
  advance(ms: number): void;
  kill(node: string): void;
  restart(node: string): void;
  split(): void;
  heal(): void;
  setRate(playersPerSecond: number): void;
  snapshot(): string;
  delete(): void; // frees the C++ object
}

export interface SimModule {
  Cluster: new (seed: number, unixStartMs: number, playersPerSecond: number) => SimEngine;
}

/**
 * Loads the WebAssembly build of the simulator. It's a separate file
 * (sim/matchmesh-sim.mjs, copied in by CI from build/wasm/demo), fetched at
 * runtime rather than bundled, so the URL is resolved against the page's
 * base href.
 */
export const SIM_LOADER = new InjectionToken<() => Promise<SimModule>>('SIM_LOADER', {
  providedIn: 'root',
  factory: () => async () => {
    const url = new URL('sim/matchmesh-sim.mjs', document.baseURI).href;
    const { default: create } = await import(/* @vite-ignore */ url);
    return (await create()) as SimModule;
  },
});

/** Calls back once per display frame; tests replace it to step by hand. */
export const FRAME_SCHEDULER = new InjectionToken<(callback: (now: number) => void) => void>(
  'FRAME_SCHEDULER',
  {
    providedIn: 'root',
    factory: () => (callback) => requestAnimationFrame(callback),
  },
);

/** Players per second when the page opens. */
export const DEFAULT_RATE = 25;
/** How often the page redraws from a fresh snapshot (the gateway polls at 5 Hz too). */
export const SNAPSHOT_EVERY_MS = 200;
/** A frame longer than this (a hidden tab, a debugger) only moves time on this much. */
export const MAX_FRAME_MS = 250;

/**
 * The cluster simulated in this browser tab: the real C++ core, compiled to
 * WebAssembly, driven in step with the display. Time in the simulation moves
 * at the same speed as on the wall clock while the tab is visible, and
 * pauses while it isn't.
 */
@Injectable({ providedIn: 'root' })
export class SimulatedCluster extends ClusterFeed {
  private readonly load = inject(SIM_LOADER);
  private readonly frame = inject(FRAME_SCHEDULER);

  override readonly state = signal<ConnectionState>('connecting');
  override readonly snapshot = signal<Snapshot | null>(null);
  readonly rate = signal(DEFAULT_RATE);

  private module: SimModule | null = null;
  private engine: SimEngine | null = null;
  private lastFrame: number | null = null;
  private sinceSnapshot = 0;
  private stopped = false;

  constructor() {
    super();
    inject(DestroyRef).onDestroy(() => {
      this.stopped = true;
      this.engine?.delete();
      this.engine = null;
    });
    this.load().then(
      (module) => {
        if (this.stopped) {
          return;
        }
        this.module = module;
        this.reset();
        this.state.set('simulated');
        this.frame((now) => this.onFrame(now));
      },
      (error: unknown) => {
        console.error('The simulator failed to load', error);
        this.state.set('failed');
      },
    );
  }

  /** Moves the simulation forward by `ms` and redraws if a snapshot is due. */
  advance(ms: number): void {
    if (!this.engine) {
      return;
    }
    this.engine.advance(ms);
    this.sinceSnapshot += ms;
    if (this.sinceSnapshot >= SNAPSHOT_EVERY_MS) {
      this.publish();
    }
  }

  kill(node: string): void {
    this.act((engine) => engine.kill(node));
  }

  restart(node: string): void {
    this.act((engine) => engine.restart(node));
  }

  split(): void {
    this.act((engine) => engine.split());
  }

  heal(): void {
    this.act((engine) => engine.heal());
  }

  setRate(playersPerSecond: number): void {
    this.rate.set(playersPerSecond);
    this.act((engine) => engine.setRate(playersPerSecond));
  }

  /** Throws the cluster away and starts five fresh nodes. */
  reset(): void {
    if (!this.module) {
      return;
    }
    this.engine?.delete();
    const seed = Math.floor(Math.random() * 2 ** 32);
    this.engine = new this.module.Cluster(seed, Date.now(), this.rate());
    this.publish();
  }

  private onFrame(now: number): void {
    if (this.stopped) {
      return;
    }
    if (this.lastFrame !== null) {
      this.advance(Math.min(now - this.lastFrame, MAX_FRAME_MS));
    }
    this.lastFrame = now;
    this.frame((next) => this.onFrame(next));
  }

  // Applies a control and redraws straight away, so the click feels instant.
  private act(change: (engine: SimEngine) => void): void {
    if (this.engine) {
      change(this.engine);
      this.publish();
    }
  }

  private publish(): void {
    if (this.engine) {
      this.sinceSnapshot = 0;
      this.snapshot.set(JSON.parse(this.engine.snapshot()) as Snapshot);
    }
  }
}

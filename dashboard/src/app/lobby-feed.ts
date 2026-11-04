import { ChangeDetectionStrategy, Component, computed, input } from '@angular/core';
import { toSignal } from '@angular/core/rxjs-interop';
import { interval, map } from 'rxjs';

import { colourFor } from './ring-geometry';
import { Lobby } from './topology';

/** "just now", "12 s ago", "3 min ago". */
export function ago(thenMs: number, nowMs: number): string {
  const seconds = Math.max(0, Math.round((nowMs - thenMs) / 1000));
  if (seconds < 2) {
    return 'just now';
  }
  if (seconds < 60) {
    return `${seconds} s ago`;
  }
  return `${Math.floor(seconds / 60)} min ago`;
}

/** How many of the newest lobbies to show. */
export const FEED_LENGTH = 10;

/** The most recently formed lobbies, newest first, with their four players. */
@Component({
  selector: 'mm-lobby-feed',
  changeDetection: ChangeDetectionStrategy.OnPush,
  template: `
    <h2>Lobbies forming</h2>
    @if (rows().length === 0) {
      <p class="empty">
        @if (hint(); as text) {
          {{ text }}
        } @else {
          No lobbies yet. Send players in with the load generator:
          <code>docker compose run --rm loadgen</code>
        }
      </p>
    } @else {
      <ol>
        @for (lobby of rows(); track lobby.id) {
          <li>
            <p class="meta">
              <span class="swatch" [style.background]="lobby.colour"></span>
              <span class="queue">{{ lobby.queue }}</span>
              <span class="when">{{ lobby.when }}</span>
            </p>
            <p class="players">
              @for (player of lobby.players; track player.id) {
                <span class="player"
                  >{{ player.id }} <b>{{ player.rating }}</b></span
                >
              }
            </p>
          </li>
        }
      </ol>
    }
  `,
  styleUrl: './lobby-feed.css',
})
export class LobbyFeed {
  readonly lobbies = input.required<Lobby[]>();
  readonly nodeIds = input.required<string[]>();
  /** The clock lobbies are stamped with, if not the wall clock (the demo's simulated one). */
  readonly clock = input<number | null>(null);
  /** Replaces the load-generator hint shown before any lobby has formed. */
  readonly hint = input<string | null>(null);

  // Ticks once a second so "12 s ago" stays true.
  private readonly wallClock = toSignal(interval(1000).pipe(map(() => Date.now())), {
    initialValue: Date.now(),
  });

  protected readonly rows = computed(() =>
    this.lobbies()
      .slice(0, FEED_LENGTH)
      .map((lobby) => ({
        ...lobby,
        colour: colourFor(this.nodeIds(), lobby.node),
        when: ago(lobby.formed_at, this.clock() ?? this.wallClock()),
      })),
  );
}

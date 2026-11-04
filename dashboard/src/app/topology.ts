// The cluster snapshot the gateway pushes over its WebSocket (see
// gateway/src/snapshot.cpp, which builds it). One document per change. The
// in-browser demo's simulator (sim/src/cluster.cpp) sends the same shape,
// plus the optional fields marked "demo only".

export interface ClusterNode {
  id: string;
  address: string;
  reachable: boolean; // the gateway could ask it this round
  member: boolean; // the current membership view counts it in
  queues: number; // queues it is primary for
  queued: number; // players waiting in those queues
  // Demo only (see sim/src/cluster.cpp):
  epoch?: number; // the view this node itself routes by
  side?: number; // during a network split, 1 or 2; otherwise 0
}

export interface RingPoint {
  node: string;
  at: number; // position on the ring, 0 (top) to 1, clockwise
}

export interface Shard {
  queue: string; // "us-west/ranked/2"
  region: string;
  mode: string;
  bracket: number;
  primary: string;
  backup: string; // empty when there is no other node
  queued: number;
  at: number; // where the queue hashes to on the ring
  contested?: boolean; // demo only: two nodes serve it at once (split brain)
}

export interface LobbyPlayer {
  id: string;
  rating: number;
}

export interface Lobby {
  id: string;
  queue: string;
  node: string; // the node that formed it
  formed_at: number; // milliseconds since the Unix epoch
  players: LobbyPlayer[];
}

/** Checked live by the simulator across every node's copies. */
export interface SimInvariants {
  acknowledged: number; // players told "you're in"
  in_lobby: number;
  queued: number;
  lost: number; // acknowledged, but held by no live node
  in_two_lobbies: number;
  contested_queues: number; // queues served on both sides of a split
  retries: number;
}

export interface SimEvent {
  seq: number;
  at: number; // same clock as Lobby.formed_at
  kind: 'action' | 'view' | 'detect' | 'handover' | 'promote' | 'warn';
  text: string;
}

/** The extra section the in-browser simulator adds. */
export interface SimSection {
  now: number; // the simulation's clock, in the same units as formed_at
  elapsed_ms: number;
  rate: number; // players arriving per second
  split: boolean;
  waiting_clients: number; // players still retrying for "you're in"
  invariants: SimInvariants;
  events: SimEvent[]; // oldest first
}

export interface Snapshot {
  epoch: number;
  coordinator: string;
  members: number;
  queued_players: number;
  nodes: ClusterNode[];
  ring: RingPoint[];
  shards: Shard[];
  lobbies: Lobby[];
  sim?: SimSection; // only from the simulator
}

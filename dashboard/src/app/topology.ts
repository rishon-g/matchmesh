// The cluster snapshot the gateway pushes over its WebSocket (see
// gateway/src/snapshot.cpp, which builds it). One document per change.

export interface ClusterNode {
  id: string;
  address: string;
  reachable: boolean; // the gateway could ask it this round
  member: boolean; // the current membership view counts it in
  queues: number; // queues it is primary for
  queued: number; // players waiting in those queues
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

export interface Snapshot {
  epoch: number;
  coordinator: string;
  members: number;
  queued_players: number;
  nodes: ClusterNode[];
  ring: RingPoint[];
  shards: Shard[];
  lobbies: Lobby[];
}

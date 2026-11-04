// Pure geometry for drawing the hash ring. Positions are fractions of the
// way round (0 at the top, increasing clockwise), as the gateway sends them.

import { RingPoint } from './topology';

/** A stretch of the ring owned by one node: keys landing in (from, to] belong to it. */
export interface Segment {
  node: string;
  from: number;
  to: number;
}

/**
 * Splits the ring into the stretches each node owns. A key belongs to the
 * first point at or after it, so each point owns the gap since the previous
 * point; the first point also owns the wrap-around gap after the last one.
 * Neighbouring stretches with the same owner are merged.
 */
export function ownership(points: readonly RingPoint[]): Segment[] {
  if (points.length === 0) {
    return [];
  }
  const segments: Segment[] = [];
  const add = (node: string, from: number, to: number) => {
    const last = segments.at(-1);
    if (last && last.node === node && last.to === from) {
      last.to = to;
    } else if (to > from) {
      segments.push({ node, from, to });
    }
  };
  const first = points[0];
  add(first.node, 0, first.at);
  for (let i = 1; i < points.length; i++) {
    add(points[i].node, points[i - 1].at, points[i].at);
  }
  add(first.node, points[points.length - 1].at, 1); // wraps round to the first point
  return segments;
}

/** The point `radius` from the centre at position `at` (0 = top, clockwise). */
export function polar(at: number, radius: number): { x: number; y: number } {
  const angle = at * 2 * Math.PI - Math.PI / 2;
  return { x: radius * Math.cos(angle), y: radius * Math.sin(angle) };
}

/** SVG path for a band between two radii, from `from` to `to` round the ring. */
export function bandPath(from: number, to: number, inner: number, outer: number): string {
  const span = Math.min(to - from, 0.999_999); // a full circle needs two arcs; never happens in practice
  const large = span > 0.5 ? 1 : 0;
  const a = polar(from, outer);
  const b = polar(from + span, outer);
  const c = polar(from + span, inner);
  const d = polar(from, inner);
  const f = (n: number) => n.toFixed(2);
  return (
    `M${f(a.x)} ${f(a.y)}A${outer} ${outer} 0 ${large} 1 ${f(b.x)} ${f(b.y)}` +
    `L${f(c.x)} ${f(c.y)}A${inner} ${inner} 0 ${large} 0 ${f(d.x)} ${f(d.y)}Z`
  );
}

/** Dot size for a queue: area grows with players waiting; an empty queue is still visible. */
export function dotRadius(queued: number, mostQueued: number): number {
  if (mostQueued <= 0 || queued <= 0) {
    return 2.5;
  }
  return 2.5 + 7.5 * Math.sqrt(Math.min(queued, mostQueued) / mostQueued);
}

/**
 * Okabe–Ito colours: chosen to stay distinguishable for the common kinds of
 * colour blindness. Nodes take them in the order the gateway lists nodes.
 */
export const NODE_COLOURS = ['#0072B2', '#E69F00', '#009E73', '#CC79A7', '#56B4E9', '#D55E00', '#F0E442'];

export function colourFor(nodeIds: readonly string[], id: string): string {
  const index = nodeIds.indexOf(id);
  return index < 0 ? '#8A97A8' : NODE_COLOURS[index % NODE_COLOURS.length];
}

import { bandPath, colourFor, dotRadius, NODE_COLOURS, ownership, polar } from './ring-geometry';

describe('ownership', () => {
  it('gives each point the gap before it, and the first point the wrap-around gap', () => {
    const segments = ownership([
      { node: 'a', at: 0.2 },
      { node: 'b', at: 0.5 },
      { node: 'c', at: 0.9 },
    ]);
    expect(segments).toEqual([
      { node: 'a', from: 0, to: 0.2 },
      { node: 'b', from: 0.2, to: 0.5 },
      { node: 'c', from: 0.5, to: 0.9 },
      { node: 'a', from: 0.9, to: 1 }, // past the last point, keys wrap round to the first
    ]);
  });

  it('merges neighbouring stretches with the same owner', () => {
    const segments = ownership([
      { node: 'a', at: 0.1 },
      { node: 'a', at: 0.3 },
      { node: 'b', at: 0.6 },
    ]);
    expect(segments.map((s) => [s.node, s.from, s.to])).toEqual([
      ['a', 0, 0.3],
      ['b', 0.3, 0.6],
      ['a', 0.6, 1],
    ]);
  });

  it('covers the whole ring exactly once', () => {
    const points = Array.from({ length: 50 }, (_, i) => ({ node: `n${i % 3}`, at: (i + 0.5) / 50 }));
    const total = ownership(points).reduce((sum, s) => sum + (s.to - s.from), 0);
    expect(total).toBeCloseTo(1, 10);
  });

  it('is empty for an empty ring', () => {
    expect(ownership([])).toEqual([]);
  });
});

describe('polar', () => {
  it('puts 0 at the top and goes clockwise', () => {
    const top = polar(0, 10);
    const right = polar(0.25, 10);
    expect(top.x).toBeCloseTo(0);
    expect(top.y).toBeCloseTo(-10);
    expect(right.x).toBeCloseTo(10);
    expect(right.y).toBeCloseTo(0);
  });
});

describe('bandPath', () => {
  it('uses the large-arc flag only for stretches over half the ring', () => {
    expect(bandPath(0, 0.2, 5, 10)).toContain('A10 10 0 0 1');
    expect(bandPath(0, 0.7, 5, 10)).toContain('A10 10 0 1 1');
  });
});

describe('dotRadius', () => {
  it('keeps empty queues visible and grows with area, not radius', () => {
    expect(dotRadius(0, 40)).toBe(2.5);
    const quarter = dotRadius(10, 40) - 2.5;
    const full = dotRadius(40, 40) - 2.5;
    expect(quarter / full).toBeCloseTo(0.5); // a quarter of the players, half the extra radius
  });
});

describe('colourFor', () => {
  it('gives nodes colours in list order and greys out strangers', () => {
    const ids = ['node-a', 'node-b'];
    expect(colourFor(ids, 'node-b')).toBe(NODE_COLOURS[1]);
    expect(colourFor(ids, 'node-z')).toBe('#8A97A8');
  });
});

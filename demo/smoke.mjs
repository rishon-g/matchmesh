// Smoke test for the WebAssembly build: loads the module the website will
// load and checks the headline behaviour end to end.
//
//   node demo/smoke.mjs build/wasm/demo/matchmesh-sim.mjs

import assert from 'node:assert/strict';
import { pathToFileURL } from 'node:url';

const { default: create } = await import(pathToFileURL(process.argv[2]).href);
const sim = new (await create()).Cluster(11, Date.now(), 40);
const snapshot = () => JSON.parse(sim.snapshot());

sim.advance(5000);
let s = snapshot();
assert.equal(s.members, 5, 'five members once started');
assert.ok(s.sim.invariants.acknowledged > 100, 'players are being admitted');

sim.kill('node-c');
sim.advance(4000);
s = snapshot();
assert.equal(s.members, 4, 'the killed node is dropped from the view');
assert.equal(s.sim.invariants.lost, 0, 'one node failing loses nobody');
assert.equal(s.sim.invariants.in_two_lobbies, 0);

sim.restart('node-c');
sim.split();
sim.advance(5000);
assert.ok(snapshot().sim.invariants.contested_queues > 0, 'a split shows split brain');

sim.heal();
sim.advance(5000);
s = snapshot();
assert.equal(s.members, 5, 'everyone is back after healing');
assert.equal(s.sim.invariants.contested_queues, 0);

sim.delete();
console.log('demo smoke test passed:', JSON.stringify(s.sim.invariants));

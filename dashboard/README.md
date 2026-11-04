# MatchMesh dashboard

The live view of the cluster: the consistent hash ring (who owns which stretch,
and every queue at the position it hashes to), each node's health, and lobbies
as they form. It listens to the gateway (`gateway/`) over a WebSocket at `/ws`
using RxJS's `webSocket`, and reconnects by itself if the gateway goes away.

Built with Angular 22 (standalone components, signals, zoneless change
detection) and tested with Vitest.

```bash
npm ci
npm start          # http://localhost:4200, proxying /ws and /api to a gateway on :8081
npm test           # unit tests
npm run build      # production build in dist/dashboard/browser
```

In Docker Compose the built app is served by nginx (`Dockerfile`, `nginx.conf`),
which also forwards `/ws` and `/api` to the gateway: open http://localhost:8080.

| File | What it does |
|---|---|
| `src/app/topology.ts` | The snapshot's shape, as the gateway sends it |
| `src/app/topology-stream.ts` | The WebSocket subscription, as a signal, with reconnects |
| `src/app/ring-geometry.ts` | Who owns which stretch of the ring, and the SVG geometry |
| `src/app/ring-chart.ts` | The ring |
| `src/app/node-list.ts` | Node health and load |
| `src/app/lobby-feed.ts` | The newest lobbies |

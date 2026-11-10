// server.js — the Node arm of KR-243: Node's own `net` and `http`, as one process or as `cluster` workers.
//
// Usage: node server.js <echo|http|skew> <port> [workers]
//   skew     each 64-byte request answered after the rounds of mixing it asks for (client.c says how)
//   workers  1 (the default) runs one process — one event loop, Node's usual shape; N > 1 forks N `cluster` workers
//            sharing the port, Node's own answer to more than one core
//   BENCH_DEADLINE_MS (environment)  an idle limit, as a Node server sets one: `socket.setTimeout`, an idle timer every
//            read refreshes, and `http`'s own header and keep-alive timeouts, checked on its sweep (30 s, or the limit
//            when shorter). Unset or 0 leaves them off.
'use strict';
const cluster = require('cluster');
const net = require('net');
const http = require('http');

const [mode, portArg, workersArg] = process.argv.slice(2);
const port = Number(portArg);
const workers = Number(workersArg || 1);
const BODY = 'Hello, World!';
const deadline = Number(process.env.BENCH_DEADLINE_MS || 0);
function arm(sock) { if (deadline > 0) sock.setTimeout(deadline, () => sock.destroy()); }

// The skew burst, the same in every arm: `rounds` passes of FNV-1a over the request's 64 bytes.
function mix(b, rounds) {
    let h = 2166136261 | 0;
    for (let r = 0; r < rounds; r++) for (let i = 0; i < 64; i++) h = Math.imul(h ^ b[i], 16777619);
    return h >>> 0;
}

function serve() {
    if (mode === 'echo') {
        net.createServer({ noDelay: true }, (sock) => {
            arm(sock);
            sock.on('data', (d) => sock.write(d));
            sock.on('error', () => sock.destroy());
        }).listen({ port, host: '127.0.0.1', backlog: 4096 });
    } else if (mode === 'skew') {
        net.createServer({ noDelay: true }, (sock) => {
            arm(sock);
            let pending = Buffer.alloc(0);
            sock.on('data', (d) => {
                pending = pending.length ? Buffer.concat([pending, d]) : d;
                while (pending.length >= 64) {
                    const req = Buffer.from(pending.subarray(0, 64));
                    pending = pending.subarray(64);
                    req.writeUInt32LE(mix(req, req.readUInt32LE(0)), 4);
                    sock.write(req);
                }
            });
            sock.on('error', () => sock.destroy());
        }).listen({ port, host: '127.0.0.1', backlog: 4096 });
    } else if (mode === 'http') {
        const options = deadline > 0 ? { connectionsCheckingInterval: Math.min(30000, deadline) } : {};
        const server = http.createServer(options, (req, res) => {
            res.writeHead(200, { 'Content-Type': 'text/plain', 'Content-Length': BODY.length });
            res.end(BODY);
        });
        server.keepAliveTimeout = deadline;   // 0: the client decides when a connection ends
        server.headersTimeout = deadline;
        server.requestTimeout = 0;
        server.listen({ port, host: '127.0.0.1', backlog: 4096 });
    } else {
        console.error(`server.js: unknown mode '${mode}'`);
        process.exit(64);
    }
}

if (workers > 1 && cluster.isPrimary) {
    for (let i = 0; i < workers; i++) cluster.fork();
} else {
    serve();
}

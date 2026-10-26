// server.js — the Node arm of KR-243: Node's own `net` and `http`, as one process or as `cluster` workers.
//
// Usage: node server.js <echo|http> <port> [workers]
//   workers  1 (the default) runs one process — one event loop, Node's usual shape; N > 1 forks N `cluster` workers
//            sharing the port, Node's own answer to more than one core
'use strict';
const cluster = require('cluster');
const net = require('net');
const http = require('http');

const [mode, portArg, workersArg] = process.argv.slice(2);
const port = Number(portArg);
const workers = Number(workersArg || 1);
const BODY = 'Hello, World!';

function serve() {
    if (mode === 'echo') {
        net.createServer({ noDelay: true }, (sock) => {
            sock.on('data', (d) => sock.write(d));
            sock.on('error', () => sock.destroy());
        }).listen({ port, host: '127.0.0.1', backlog: 4096 });
    } else if (mode === 'http') {
        const server = http.createServer((req, res) => {
            res.writeHead(200, { 'Content-Type': 'text/plain', 'Content-Length': BODY.length });
            res.end(BODY);
        });
        server.keepAliveTimeout = 0;        // the client decides when a connection ends
        server.headersTimeout = 0;
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

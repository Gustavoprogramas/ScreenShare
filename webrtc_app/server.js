const express = require('express');
const https = require('https');
const selfsigned = require('selfsigned');
const { Server } = require('socket.io');

const app = express();
app.use(express.static('public'));

// Track active sharers: socketId -> { name, resolution, fps }
const sharers = new Map();

(async () => {
    const attrs = [{ name: 'commonName', value: 'localhost' }];
    const pems = await selfsigned.generate(attrs, { 
        keySize: 2048, 
        days: 365,
        algorithm: 'sha256'
    });

    const server = https.createServer({
        key: pems.private,
        cert: pems.cert
    }, app);
    const io = new Server(server, {
        cors: { origin: "*", methods: ["GET", "POST"] }
    });

    function broadcastSharerList() {
        const list = [];
        for (const [id, info] of sharers) {
            list.push({ id, name: info.name, resolution: info.resolution, fps: info.fps });
        }
        io.to('remote-desktop').emit('sharer-list', list);
    }

    io.on('connection', (socket) => {
        console.log(`User connected: ${socket.id}`);
        socket.join('remote-desktop');

        // Send current sharer list to the new user
        const list = [];
        for (const [id, info] of sharers) {
            list.push({ id, name: info.name, resolution: info.resolution, fps: info.fps });
        }
        socket.emit('sharer-list', list);

        // A user starts sharing
        socket.on('start-sharing', (data) => {
            sharers.set(socket.id, {
                name: data.name || 'Unknown',
                resolution: data.resolution || '1080p',
                fps: data.fps || '60'
            });
            console.log(`Sharer added: ${socket.id} (${data.name})`);
            broadcastSharerList();
        });

        // A user stops sharing
        socket.on('stop-sharing', () => {
            sharers.delete(socket.id);
            console.log(`Sharer removed: ${socket.id}`);
            broadcastSharerList();
        });

        // Viewer requests to watch a specific sharer
        socket.on('request-stream', (data) => {
            // Tell the sharer that this viewer wants their stream
            io.to(data.sharerId).emit('viewer-request', { viewerId: socket.id });
        });

        // Targeted signaling between two specific peers
        socket.on('signal', (data) => {
            io.to(data.targetId).emit('signal', {
                senderId: socket.id,
                signal: data.signal
            });
        });

        socket.on('disconnect', () => {
            console.log(`User disconnected: ${socket.id}`);
            if (sharers.has(socket.id)) {
                sharers.delete(socket.id);
                broadcastSharerList();
            }
        });
    });

    const PORT = process.env.PORT || 3000;
    server.listen(PORT, '0.0.0.0', () => {
        console.log(`Signaling server running on https://0.0.0.0:${PORT}`);
    });
})();

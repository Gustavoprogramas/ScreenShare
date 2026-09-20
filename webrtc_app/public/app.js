const socket = io();

// UI Elements
const btnShare = document.getElementById('btnShare');
const btnStop = document.getElementById('btnStop');
const btnFullscreen = document.getElementById('btnFullscreen');
const videoCard = document.getElementById('videoCard');
const userNameInput = document.getElementById('userName');
const resolutionSelect = document.getElementById('resolution');
const framerateSelect = document.getElementById('framerate');
const bitrateSelect = document.getElementById('bitrate');
const shareAudioCheckbox = document.getElementById('shareAudio');
const remoteVideo = document.getElementById('remoteVideo');
const videoPlaceholder = document.getElementById('videoPlaceholder');
const statusDot = document.getElementById('statusDot');
const statusText = document.getElementById('statusText');
const streamsList = document.getElementById('streamsList');

// State
let localStream = null;
let isSharing = false;
let viewerPeers = {};       // sharerId -> peer connections from viewers requesting MY stream
let watchingPeer = null;    // peer connection when I'm watching someone
let watchingSharerId = null;
let bitrateInterval = null;

function getTargetBitrate() {
    return parseInt(bitrateSelect.value) * 1000;
}

// ============================================================
//  FORCE HIGH BITRATE
// ============================================================
async function forceHighBitrate(pc) {
    for (const sender of pc.getSenders()) {
        if (sender.track && sender.track.kind === 'video') {
            const params = sender.getParameters();
            if (!params.encodings || params.encodings.length === 0) {
                params.encodings = [{}];
            }
            const enc = params.encodings[0];
            enc.maxBitrate = getTargetBitrate();
            enc.maxFramerate = parseInt(framerateSelect.value);
            enc.scaleResolutionDownBy = 1.0;
            enc.networkPriority = 'high';
            enc.priority = 'high';
            params.degradationPreference = 'disabled';
            try {
                await sender.setParameters(params);
            } catch (_) {
                params.degradationPreference = 'maintain-resolution';
                try { await sender.setParameters(params); } catch (__) {}
            }
        }
    }
}

// ============================================================
//  FULLSCREEN
// ============================================================
btnFullscreen.addEventListener('click', toggleFullscreen);
remoteVideo.addEventListener('dblclick', toggleFullscreen);

function toggleFullscreen() {
    if (!document.fullscreenElement) {
        videoCard.requestFullscreen().catch(() => {});
    } else {
        document.exitFullscreen();
    }
}

// ============================================================
//  SIGNALING
// ============================================================
socket.on('connect', () => {
    updateStatus('connected', 'Connected to Server');
});

socket.on('disconnect', () => {
    updateStatus('disconnected', 'Disconnected');
    cleanupAll();
});

// Receive updated list of active sharers
socket.on('sharer-list', (list) => {
    renderSharerList(list);
});

// I'm a sharer and a viewer wants my stream
socket.on('viewer-request', (data) => {
    if (!localStream) return;

    // Create a new peer connection for this specific viewer
    const viewerId = data.viewerId;
    if (viewerPeers[viewerId]) {
        viewerPeers[viewerId].destroy();
    }

    const sdpTransform = makeSdpTransform();
    const peer = new SimplePeer({
        initiator: true,
        stream: localStream,
        trickle: true,
        sdpTransform: sdpTransform,
        config: { iceServers: getIceServers() }
    });

    peer.on('signal', (signal) => {
        socket.emit('signal', { targetId: viewerId, signal });
    });

    peer.on('connect', () => {
        console.log(`Viewer ${viewerId} connected to my stream`);
        const pc = peer._pc;
        if (pc) {
            forceHighBitrate(pc);
            const iv = setInterval(() => {
                if (!peer || peer.destroyed) { clearInterval(iv); return; }
                forceHighBitrate(pc);
            }, 2000);
        }
    });

    peer.on('error', () => { delete viewerPeers[viewerId]; });
    peer.on('close', () => { delete viewerPeers[viewerId]; });

    viewerPeers[viewerId] = peer;
});

// Receive signal targeted at me
socket.on('signal', (data) => {
    const senderId = data.senderId;

    // Am I a sharer receiving signals from a viewer?
    if (viewerPeers[senderId]) {
        viewerPeers[senderId].signal(data.signal);
        return;
    }

    // Am I a viewer receiving signals from the sharer I'm watching?
    if (watchingSharerId === senderId && watchingPeer) {
        watchingPeer.signal(data.signal);
        return;
    }

    // If I'm a viewer and I don't have a peer yet, create one (non-initiator)
    if (watchingSharerId === senderId && !watchingPeer) {
        createViewerPeer(senderId);
        watchingPeer.signal(data.signal);
        return;
    }
});

// ============================================================
//  SHARE SCREEN
// ============================================================
btnShare.addEventListener('click', async () => {
    try {
        const height = parseInt(resolutionSelect.value);
        const fps = parseInt(framerateSelect.value);
        const width = height === 1080 ? 1920 : 1280;
        const wantAudio = shareAudioCheckbox.checked;

        const constraints = {
            video: {
                width: { ideal: width, max: 1920 },
                height: { ideal: height, max: 1080 },
                frameRate: { ideal: fps, max: 60 },
                cursor: 'always'
            },
            audio: wantAudio
        };

        localStream = await navigator.mediaDevices.getDisplayMedia(constraints);
        
        // Optimize video track
        const videoTrack = localStream.getVideoTracks()[0];
        if (videoTrack.contentHint !== undefined) {
            videoTrack.contentHint = 'detail';
        }
        try {
            await videoTrack.applyConstraints({
                width: { ideal: width },
                height: { ideal: height },
                frameRate: { ideal: fps, min: Math.min(fps, 24) }
            });
        } catch (_) {}

        // Show local preview
        remoteVideo.srcObject = localStream;
        remoteVideo.muted = true; // Mute local preview to prevent audio feedback loop
        hidePlaceholder();
        isSharing = true;
        updateStatus('sharing', `Sharing ${height}p @ ${fps}fps`);
        
        btnShare.classList.add('hidden');
        btnStop.classList.remove('hidden');

        videoTrack.onended = () => stopSharing();

        // Register as sharer on the server
        const name = userNameInput.value.trim() || `User-${socket.id.slice(0, 4)}`;
        socket.emit('start-sharing', { name, resolution: `${height}p`, fps: `${fps}` });

    } catch (err) {
        console.error('Error sharing screen:', err);
        alert('Could not start screen share. Make sure to select "Entire screen" and check "Share audio" in the browser popup.');
    }
});

btnStop.addEventListener('click', stopSharing);

function stopSharing() {
    if (localStream) {
        localStream.getTracks().forEach(t => t.stop());
        localStream = null;
    }
    isSharing = false;

    // Destroy all viewer peer connections
    for (const id in viewerPeers) {
        viewerPeers[id].destroy();
    }
    viewerPeers = {};

    remoteVideo.srcObject = null;
    showPlaceholder();
    
    btnStop.classList.add('hidden');
    btnShare.classList.remove('hidden');
    
    socket.emit('stop-sharing');
    updateStatus('connected', 'Connected (Idle)');
}

// ============================================================
//  WATCH A STREAM
// ============================================================
function watchStream(sharerId) {
    if (sharerId === socket.id) return; // Can't watch yourself

    // Cleanup previous watching
    if (watchingPeer) {
        watchingPeer.destroy();
        watchingPeer = null;
    }
    watchingSharerId = sharerId;

    // Create a non-initiator peer — the sharer will send us the offer
    createViewerPeer(sharerId);

    // Ask the server to tell the sharer we want their stream
    socket.emit('request-stream', { sharerId });
    updateStatus('connected', 'Connecting to stream...');
}

function createViewerPeer(sharerId) {
    const peer = new SimplePeer({
        initiator: false,
        trickle: true,
        config: { iceServers: getIceServers() }
    });

    peer.on('signal', (signal) => {
        socket.emit('signal', { targetId: sharerId, signal });
    });

    peer.on('stream', (stream) => {
        console.log('Received stream!');
        remoteVideo.srcObject = stream;
        remoteVideo.muted = false; // Unmute to hear the stream audio
        
        // Explicitly start playing to prevent black screens from autoplay policies
        remoteVideo.play().catch(err => {
            console.warn("Autoplay blocked with audio, falling back to muted autoplay:", err);
            remoteVideo.muted = true;
            remoteVideo.play().catch(e => console.error("Total playback failure:", e));
            // Show a tiny alert or just let them double click to unmute
            updateStatus('connected', 'Playing (Muted - Click to Unmute)');
        });

        hidePlaceholder();
        updateStatus('connected', 'Watching stream');
    });

    peer.on('error', (err) => {
        console.error('Viewer peer error:', err);
        watchingPeer = null;
        watchingSharerId = null;
        showPlaceholder();
        updateStatus('connected', 'Stream ended');
    });

    peer.on('close', () => {
        watchingPeer = null;
        watchingSharerId = null;
        showPlaceholder();
        updateStatus('connected', 'Stream ended');
    });

    watchingPeer = peer;
}

// ============================================================
//  RENDER SHARER LIST
// ============================================================
function renderSharerList(list) {
    if (list.length === 0) {
        streamsList.innerHTML = '<p class="no-streams">No active streams</p>';
        return;
    }

    streamsList.innerHTML = '';
    for (const sharer of list) {
        const div = document.createElement('div');
        div.className = 'stream-item';
        
        const isMe = sharer.id === socket.id;
        const isActive = watchingSharerId === sharer.id;

        if (isMe) div.classList.add('is-me');
        if (isActive) div.classList.add('active');

        div.innerHTML = `
            <div class="stream-info">
                <span class="stream-name">${escapeHtml(sharer.name)}${isMe ? ' (You)' : ''}</span>
                <span class="stream-meta">${sharer.resolution} · ${sharer.fps} fps</span>
            </div>
            <span class="stream-live">● LIVE</span>
        `;

        if (!isMe) {
            div.addEventListener('click', () => watchStream(sharer.id));
        }

        streamsList.appendChild(div);
    }
}

function escapeHtml(text) {
    const d = document.createElement('div');
    d.textContent = text;
    return d.innerHTML;
}

// ============================================================
//  HELPERS
// ============================================================
function getIceServers() {
    return [
        { urls: 'stun:stun.l.google.com:19302' },
        { urls: 'stun:stun1.l.google.com:19302' },
        { urls: 'stun:stun2.l.google.com:19302' },
        { urls: 'stun:global.stun.twilio.com:3478' }
    ];
}

function makeSdpTransform() {
    return (sdp) => {
        const targetKbps = Math.round(getTargetBitrate() / 1000);
        sdp = sdp.replace(/b=AS:.*\r\n/g, '');
        sdp = sdp.replace(/b=TIAS:.*\r\n/g, '');
        sdp = sdp.replace(/(m=video.*\r\n)/g, `$1b=AS:${targetKbps}\r\n`);
        return sdp;
    };
}

function cleanupAll() {
    if (watchingPeer) { watchingPeer.destroy(); watchingPeer = null; }
    watchingSharerId = null;
    for (const id in viewerPeers) { viewerPeers[id].destroy(); }
    viewerPeers = {};
}

function hidePlaceholder() {
    videoPlaceholder.style.opacity = '0';
    setTimeout(() => videoPlaceholder.style.display = 'none', 300);
}

function showPlaceholder() {
    videoPlaceholder.style.display = 'flex';
    setTimeout(() => videoPlaceholder.style.opacity = '1', 10);
}

function updateStatus(state, text) {
    statusDot.className = `status-indicator ${state}`;
    statusText.innerText = text;
}

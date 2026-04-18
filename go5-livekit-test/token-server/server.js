import express from 'express';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { AccessToken } from 'livekit-server-sdk';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);

const app = express();
app.use(express.json());

const port = Number(process.env.PORT || 3000);
const defaultRoom = process.env.DEFAULT_ROOM || 'qt-test-room';
const wsUrl = process.env.LIVEKIT_WS_URL || 'wss://go5.avcore.io';
const apiKey = process.env.LIVEKIT_API_KEY;
const apiSecret = process.env.LIVEKIT_API_SECRET;

if (!apiKey || !apiSecret) {
  console.error('Missing LIVEKIT_API_KEY or LIVEKIT_API_SECRET');
  process.exit(1);
}

app.get('/healthz', (_req, res) => {
  res.json({ ok: true, wsUrl, defaultRoom });
});

app.post('/token', async (req, res) => {
  try {
    const room = String(req.body?.room || defaultRoom).trim();
    const identity = String(req.body?.identity || '').trim();
    const role = String(req.body?.role || 'viewer').trim();

    if (!identity) {
      res.status(400).json({ error: 'identity is required' });
      return;
    }

    const at = new AccessToken(apiKey, apiSecret, {
      identity,
      ttl: '6h',
    });

    if (role === 'publisher') {
      at.addGrant({
        roomJoin: true,
        room,
        canPublish: true,
        canSubscribe: true,
      });
    } else {
      at.addGrant({
        roomJoin: true,
        room,
        canPublish: false,
        canSubscribe: true,
      });
    }

    const token = await at.toJwt();
    res.json({
      wsUrl,
      room,
      identity,
      role,
      token,
    });
  } catch (err) {
    console.error(err);
    res.status(500).json({ error: 'failed to mint token' });
  }
});

app.use('/publish', express.static(path.join(__dirname, 'public')));
app.get('/publish', (_req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

app.listen(port, '0.0.0.0', () => {
  console.log(`token/publisher app listening on ${port}`);
});

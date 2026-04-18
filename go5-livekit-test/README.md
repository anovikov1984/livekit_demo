# go5.avcore.io LiveKit test stack

This stack gives you three things on one domain:

- `wss://go5.avcore.io` -> LiveKit server
- `https://go5.avcore.io/token` -> token endpoint
- `https://go5.avcore.io/publish` -> tiny webcam publisher page

The publisher page uses one token for the browser publisher and generates a second **viewer token** for your Qt app so you do not reuse the same participant identity.

## 1. Prerequisites

- Ubuntu 24.04 server with Docker Engine + Docker Compose plugin installed
- DNS `A` record for `go5.avcore.io` pointing at the server
- AWS security group / firewall rules:
  - TCP 80
  - TCP 443
  - TCP 7881
  - UDP 7882

LiveKit's docs list the standard ports as API/WebSocket 7880 behind TLS termination, ICE/TCP 7881, and either a UDP port range or single UDP mux port 7882. This stack uses the single UDP mux port. citeturn477504view0

## 2. Prepare the env file

```bash
cd /opt
sudo mkdir -p /opt/go5-livekit-test
sudo chown $USER:$USER /opt/go5-livekit-test
```

Copy this project there, then:

```bash
cd /opt/go5-livekit-test
cp .env.example .env
nano .env
```

Set:

- `DOMAIN=go5.avcore.io`
- `ACME_EMAIL=your-real-email@example.com`
- `LIVEKIT_API_KEY=devkey`
- `LIVEKIT_API_SECRET=replace-this`
- `DEFAULT_ROOM=qt-test-room`

## 3. Start it

```bash
docker compose up -d --build
```

Check:

```bash
docker compose ps
docker compose logs -f caddy
docker compose logs -f livekit
docker compose logs -f app
```

## 4. Test it

Open:

- `https://go5.avcore.io/publish`

Enter identities if you want, then click **Start webcam publisher**.

The page will:

- mint a publisher token
- mint a separate viewer token
- connect the browser to LiveKit using the publisher token
- publish **camera only** (microphone disabled)
- show the viewer token you can paste into your Qt app

For the Qt app, use:

- `apiUrl`: `wss://go5.avcore.io`
- `token`: the value from the **Viewer token for Qt app** box

## 5. Token endpoint

`POST /token`

Example:

```bash
curl -s https://go5.avcore.io/token \
  -H 'content-type: application/json' \
  -d '{"room":"qt-test-room","identity":"qt-viewer","role":"viewer"}'
```

It returns:

```json
{
  "wsUrl": "wss://go5.avcore.io",
  "room": "qt-test-room",
  "identity": "qt-viewer",
  "role": "viewer",
  "token": "eyJ..."
}
```

## Notes

- Token generation uses `livekit-server-sdk` v2 and must `await at.toJwt()`. The SDK docs show `const token = await at.toJwt()`. citeturn477504view2
- Browser camera publishing is done through the LiveKit JS client. LiveKit docs show camera publishing with `room.localParticipant.setCameraEnabled(true)`. citeturn477504view3turn477504view4
- Caddy handles automatic Let's Encrypt certificates for `go5.avcore.io`.

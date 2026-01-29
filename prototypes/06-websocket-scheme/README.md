# Prototype 06: WebSocket over Custom Scheme

Implements WebSocket protocol framing (RFC 6455) over the saucer streaming scheme API. No actual TCP sockets are used - all communication goes through custom URL scheme handlers.

## How It Works

### Transport Layer
- **Server → Client**: Uses the streaming scheme (`wsock://localhost/connect`) for a long-lived connection
- **Client → Server**: Uses POST requests to `wsock://localhost/send`

### WebSocket Framing
Standard WebSocket frame format is used:
- FIN bit, opcode (TEXT, BINARY, PING, PONG, CLOSE)
- Payload length (7-bit, 16-bit, or 64-bit)
- Masking key (client → server only, as per spec)
- Payload data

### Supported Opcodes
- `0x1` TEXT - Text message
- `0x2` BINARY - Binary message
- `0x8` CLOSE - Connection close
- `0x9` PING - Ping frame
- `0xA` PONG - Pong frame

## Running

### Interactive Mode
```bash
./prototype_websocket_scheme
```
Opens a window with Connect/Disconnect buttons. You can send messages which will be echoed back.

### Test Mode
```bash
./prototype_websocket_scheme --test
```
Automatically:
1. Connects via the streaming scheme
2. C++ sends 3 PING frames
3. JavaScript responds with PONG frames (with matching payloads)
4. Exits with code 0 on success, 1 on failure

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                        WebView (JS)                         │
├─────────────────────────────────────────────────────────────┤
│  fetch('wsock://localhost/connect')  →  ReadableStream     │
│  fetch('wsock://localhost/send', POST) →  Send frames      │
│                                                             │
│  [WebSocket Frame Encode/Decode in JavaScript]              │
└─────────────────────────────────────────────────────────────┘
                            ↕
┌─────────────────────────────────────────────────────────────┐
│                 Streaming Scheme Handler                     │
├─────────────────────────────────────────────────────────────┤
│  /connect  →  stream_writer (long-lived)                    │
│  /send     →  Receive frames, push to queue                 │
│                                                             │
│  [WebSocket Frame Encode/Decode in C++]                     │
└─────────────────────────────────────────────────────────────┘
```

## Key Differences from Native WebSocket

1. **No TCP handshake** - Uses scheme handler directly
2. **Unidirectional streams** - Server→client via streaming, client→server via POST
3. **Custom scheme** - Uses `wsock://` instead of `ws://`
4. **Same-process** - All communication stays within the app process

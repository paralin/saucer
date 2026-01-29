# Prototype 06: WebSocket over Custom Scheme

Implements WebSocket protocol framing (RFC 6455) over the saucer streaming scheme API. No actual TCP sockets are used - all communication goes through custom URL scheme handlers and IPC.

## How It Works

### Transport Layer
- **Server → Client**: Uses the streaming scheme (`wsock://localhost/connect`) for a long-lived connection
- **Client → Server**: Uses binary message handler via `WKScriptMessageHandler.postMessage()` - direct IPC with no HTTP overhead

Both directions avoid HTTP request/response overhead per message.

### Binary Packing (Client → Server)
To efficiently transfer binary data from JavaScript to C++:
1. JavaScript packs 4 bytes into each 32-bit unsigned integer
2. Sends as `Uint32Array` → converted to NSArray of NSNumbers by WebKit
3. C++ unpacks the array back to bytes

This is ~4x more efficient than sending 1 byte per NSNumber.

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
│  window.saucer.internal.sendBinary() →  Binary IPC         │
│                                                             │
│  [WebSocket Frame Encode/Decode in JavaScript]              │
└─────────────────────────────────────────────────────────────┘
                            ↕
┌─────────────────────────────────────────────────────────────┐
│              Scheme Handler + Binary Message Handler        │
├─────────────────────────────────────────────────────────────┤
│  /connect         →  stream_writer (long-lived response)   │
│  binary_message   →  Receive frames via IPC, push to queue │
│                                                             │
│  [WebSocket Frame Encode/Decode in C++]                     │
└─────────────────────────────────────────────────────────────┘
```

## Key Differences from Native WebSocket

1. **No TCP handshake** - Uses scheme handler directly
2. **Efficient bidirectional** - Streaming for server→client, IPC for client→server
3. **Custom scheme** - Uses `wsock://` instead of `ws://`
4. **Same-process** - All communication stays within the app process
5. **No HTTP overhead** - Binary message handler bypasses HTTP entirely for client→server

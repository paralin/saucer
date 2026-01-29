// ==================================================================================
// Prototype 06: WebSocket over Custom Scheme
// ==================================================================================
// Implements WebSocket protocol framing over the custom streaming scheme.
// No actual TCP sockets - all communication goes through the scheme handler.
//
// Transport:
// - Server → Client: Streaming scheme (single long-lived connection)
// - Client → Server: Binary message handler (IPC, no HTTP overhead)
//
// Framing: Standard WebSocket frame format (RFC 6455)
//
// Test mode: Run with --test to automatically verify ping/pong exchange.
// ==================================================================================

#include <saucer/smartview.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <format>
#include <mutex>
#include <random>
#include <span>
#include <thread>
#include <vector>

// WebSocket opcodes (RFC 6455)
enum class WsOpcode : std::uint8_t
{
    CONTINUATION = 0x0,
    TEXT         = 0x1,
    BINARY       = 0x2,
    CLOSE        = 0x8,
    PING         = 0x9,
    PONG         = 0xA,
};

// WebSocket frame
struct WsFrame
{
    bool fin{true};
    WsOpcode opcode{WsOpcode::TEXT};
    std::vector<std::uint8_t> payload;

    // Encode frame to wire format (server → client, no masking)
    std::vector<std::uint8_t> encode() const
    {
        std::vector<std::uint8_t> data;

        // First byte: FIN + opcode
        std::uint8_t byte0 = (fin ? 0x80 : 0x00) | static_cast<std::uint8_t>(opcode);
        data.push_back(byte0);

        // Second byte: MASK=0 + payload length
        if (payload.size() <= 125)
        {
            data.push_back(static_cast<std::uint8_t>(payload.size()));
        }
        else if (payload.size() <= 65535)
        {
            data.push_back(126);
            data.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xFF));
            data.push_back(static_cast<std::uint8_t>(payload.size() & 0xFF));
        }
        else
        {
            data.push_back(127);
            for (int i = 7; i >= 0; i--)
            {
                data.push_back(static_cast<std::uint8_t>((payload.size() >> (i * 8)) & 0xFF));
            }
        }

        // Payload (no masking for server → client)
        data.insert(data.end(), payload.begin(), payload.end());
        return data;
    }

    // Decode frame from wire format (client → server, masked)
    static std::optional<std::pair<WsFrame, std::size_t>> decode(const std::uint8_t *data, std::size_t size)
    {
        if (size < 2)
            return std::nullopt;

        WsFrame frame;
        std::size_t offset = 0;

        // First byte
        frame.fin    = (data[0] & 0x80) != 0;
        frame.opcode = static_cast<WsOpcode>(data[0] & 0x0F);
        offset++;

        // Second byte
        bool masked          = (data[1] & 0x80) != 0;
        std::uint64_t length = data[1] & 0x7F;
        offset++;

        // Extended length
        if (length == 126)
        {
            if (size < offset + 2)
                return std::nullopt;
            length = (static_cast<std::uint64_t>(data[offset]) << 8) | static_cast<std::uint64_t>(data[offset + 1]);
            offset += 2;
        }
        else if (length == 127)
        {
            if (size < offset + 8)
                return std::nullopt;
            length = 0;
            for (int i = 0; i < 8; i++)
            {
                length = (length << 8) | static_cast<std::uint64_t>(data[offset + i]);
            }
            offset += 8;
        }

        // Masking key
        std::uint8_t mask[4] = {0, 0, 0, 0};
        if (masked)
        {
            if (size < offset + 4)
                return std::nullopt;
            std::memcpy(mask, data + offset, 4);
            offset += 4;
        }

        // Payload
        if (size < offset + length)
            return std::nullopt;

        frame.payload.resize(length);
        for (std::size_t i = 0; i < length; i++)
        {
            frame.payload[i] = data[offset + i] ^ mask[i % 4];
        }
        offset += length;

        return std::make_pair(std::move(frame), offset);
    }
};

// Thread-safe frame queue
class FrameQueue
{
    std::deque<WsFrame> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_closed{false};

  public:
    void push(WsFrame frame)
    {
        {
            std::lock_guard lock(m_mutex);
            m_queue.push_back(std::move(frame));
        }
        m_cv.notify_one();
    }

    void close()
    {
        m_closed = true;
        m_cv.notify_all();
    }

    void reset()
    {
        std::lock_guard lock(m_mutex);
        m_queue.clear();
        m_closed = false;
    }

    std::optional<WsFrame> wait_pop(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(m_mutex);
        if (!m_cv.wait_for(lock, timeout, [this] { return !m_queue.empty() || m_closed; }))
        {
            return std::nullopt;
        }
        if (m_queue.empty())
        {
            return std::nullopt;
        }
        auto frame = std::move(m_queue.front());
        m_queue.pop_front();
        return frame;
    }
};

// Global state
FrameQueue g_from_client;
std::atomic<bool> g_running{false};
std::atomic<bool> g_connected{false};
std::atomic<bool> g_test_mode{false};
std::atomic<bool> g_dom_ready{false};
std::atomic<int> g_test_result{1};
std::atomic<int> g_pings_sent{0};
std::atomic<int> g_pongs_received{0};
saucer::application *g_app{nullptr};

// WebSocket server thread - sends frames to client via streaming scheme
void websocket_server_thread(saucer::scheme::stream_writer writer)
{
    writer.start({
        .mime    = "application/octet-stream",
        .headers = {
            {"Access-Control-Allow-Origin", "*"},
            {"Cache-Control", "no-cache"},
        },
    });

    g_connected = true;
    std::fprintf(stderr, "[WS] Client connected via streaming scheme\n");

    if (g_test_mode)
    {
        constexpr int PING_COUNT            = 3;
        constexpr auto PING_TIMEOUT         = std::chrono::milliseconds(3000);

        for (int i = 0; i < PING_COUNT && g_running && writer.valid(); i++)
        {
            std::string ping_data = std::format("ping-{}", i);
            WsFrame ping{
                .opcode  = WsOpcode::PING,
                .payload = {ping_data.begin(), ping_data.end()},
            };

            auto encoded = ping.encode();
            writer.write(saucer::stash::from(encoded));
            g_pings_sent++;
            std::fprintf(stderr, "[WS] Sent PING: %s\n", ping_data.c_str());

            // Wait for PONG
            auto response = g_from_client.wait_pop(PING_TIMEOUT);
            if (!response || response->opcode != WsOpcode::PONG)
            {
                std::fprintf(stderr, "[TEST] FAILED: No PONG received for %s\n", ping_data.c_str());
                g_test_result = 1;
                break;
            }

            std::string pong_data(response->payload.begin(), response->payload.end());
            if (pong_data != ping_data)
            {
                std::fprintf(stderr, "[TEST] FAILED: PONG mismatch. Expected '%s', got '%s'\n", ping_data.c_str(),
                             pong_data.c_str());
                g_test_result = 1;
                break;
            }

            g_pongs_received++;
            std::fprintf(stderr, "[WS] PONG received: %s\n", pong_data.c_str());

            if (i == PING_COUNT - 1)
            {
                g_test_result = 0;
                std::fprintf(stderr, "[TEST] SUCCESS: All %d ping/pong exchanges completed\n", PING_COUNT);
            }
        }

        // Send CLOSE frame
        WsFrame close_frame{.opcode = WsOpcode::CLOSE, .payload = {0x03, 0xE8}}; // 1000 = normal closure
        writer.write(saucer::stash::from(close_frame.encode()));

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        g_running = false;
        writer.finish();

        if (g_app)
            g_app->quit();
        return;
    }

    // Interactive mode
    while (g_running && writer.valid())
    {
        auto frame = g_from_client.wait_pop(std::chrono::milliseconds(100));
        if (frame)
        {
            if (frame->opcode == WsOpcode::TEXT || frame->opcode == WsOpcode::BINARY)
            {
                // Echo back
                writer.write(saucer::stash::from(frame->encode()));
                std::fprintf(stderr, "[WS] Echoed: %s\n",
                             std::string(frame->payload.begin(), frame->payload.end()).c_str());
            }
            else if (frame->opcode == WsOpcode::PONG)
            {
                g_pongs_received++;
            }
            else if (frame->opcode == WsOpcode::CLOSE)
            {
                break;
            }
        }
    }

    WsFrame close_frame{.opcode = WsOpcode::CLOSE, .payload = {0x03, 0xE8}};
    writer.write(saucer::stash::from(close_frame.encode()));
    writer.finish();
    g_connected = false;
}

static constexpr auto html_template = R"html(
<!DOCTYPE html>
<html>
<head>
    <meta charset="UTF-8">
    <title>WebSocket over Custom Scheme</title>
    <style>
        body { font-family: monospace; padding: 20px; background: #1a1a2e; color: #eee; }
        .stats { margin: 20px 0; }
        .stat { margin: 5px 0; }
        button { margin: 5px; padding: 10px 20px; cursor: pointer; }
        input { padding: 8px; width: 300px; font-family: monospace; }
        #log { background: #16213e; padding: 10px; height: 300px; overflow-y: auto; border-radius: 4px; }
        .success { color: #51cf66; }
        .error { color: #ff6b6b; }
        .info { color: #74c0fc; }
        .ping { color: #ffd43b; }
    </style>
</head>
<body>
    <h1>WebSocket over Custom Scheme</h1>
    <p>WebSocket framing over streaming scheme - no TCP sockets!</p>

    <div>
        <button onclick="connect()">Connect</button>
        <button onclick="disconnect()">Disconnect</button>
    </div>

    <div style="margin: 10px 0;">
        <input type="text" id="message" placeholder="Type a message..." onkeypress="if(event.key==='Enter')sendMessage()">
        <button onclick="sendMessage()">Send</button>
    </div>

    <div class="stats">
        <div class="stat">Status: <span id="status">Disconnected</span></div>
        <div class="stat">Messages sent: <span id="sent">0</span></div>
        <div class="stat">Messages received: <span id="received">0</span></div>
        <div class="stat">Pings received: <span id="pings">0</span></div>
        <div class="stat">Pongs sent: <span id="pongs">0</span></div>
    </div>

    <div id="log"></div>

    <script>
        const OPCODE = { TEXT: 0x1, BINARY: 0x2, CLOSE: 0x8, PING: 0x9, PONG: 0xA };
        let reader = null;
        let connected = false;
        let buffer = new Uint8Array(0);
        let stats = { sent: 0, received: 0, pings: 0, pongs: 0 };

        function log(msg, cls = '') {
            const el = document.getElementById('log');
            el.innerHTML += `<div class="${cls}">[${new Date().toLocaleTimeString()}] ${msg}</div>`;
            el.scrollTop = el.scrollHeight;
        }

        function updateStats() {
            document.getElementById('sent').textContent = stats.sent;
            document.getElementById('received').textContent = stats.received;
            document.getElementById('pings').textContent = stats.pings;
            document.getElementById('pongs').textContent = stats.pongs;
        }

        // Encode WebSocket frame (client → server, with masking)
        function encodeFrame(opcode, payload) {
            const payloadBytes = typeof payload === 'string' ? new TextEncoder().encode(payload) : payload;
            const len = payloadBytes.length;

            let headerLen = 2 + 4; // base + mask
            if (len > 125 && len <= 65535) headerLen += 2;
            else if (len > 65535) headerLen += 8;

            const frame = new Uint8Array(headerLen + len);
            let offset = 0;

            // FIN + opcode
            frame[offset++] = 0x80 | opcode;

            // MASK=1 + length
            if (len <= 125) {
                frame[offset++] = 0x80 | len;
            } else if (len <= 65535) {
                frame[offset++] = 0x80 | 126;
                frame[offset++] = (len >> 8) & 0xFF;
                frame[offset++] = len & 0xFF;
            } else {
                frame[offset++] = 0x80 | 127;
                for (let i = 7; i >= 0; i--) frame[offset++] = (len >> (i * 8)) & 0xFF;
            }

            // Masking key
            const mask = new Uint8Array(4);
            crypto.getRandomValues(mask);
            frame.set(mask, offset);
            offset += 4;

            // Masked payload
            for (let i = 0; i < len; i++) {
                frame[offset + i] = payloadBytes[i] ^ mask[i % 4];
            }

            return frame;
        }

        // Decode WebSocket frame (server → client, no masking)
        function decodeFrame(data) {
            if (data.length < 2) return null;

            const fin = (data[0] & 0x80) !== 0;
            const opcode = data[0] & 0x0F;
            const masked = (data[1] & 0x80) !== 0;
            let len = data[1] & 0x7F;
            let offset = 2;

            if (len === 126) {
                if (data.length < 4) return null;
                len = (data[2] << 8) | data[3];
                offset = 4;
            } else if (len === 127) {
                if (data.length < 10) return null;
                len = 0;
                for (let i = 0; i < 8; i++) len = (len << 8) | data[2 + i];
                offset = 10;
            }

            if (masked) offset += 4; // Skip mask (server shouldn't mask)
            if (data.length < offset + len) return null;

            return {
                fin, opcode,
                payload: data.slice(offset, offset + len),
                totalLength: offset + len
            };
        }

        function sendFrame(opcode, payload) {
            const frame = encodeFrame(opcode, payload);
            // Use binary message IPC instead of POST
            window.saucer.internal.sendBinary(frame);
        }

        async function connect() {
            if (connected) { log('Already connected!', 'error'); return; }

            log('Connecting...', 'info');
            document.getElementById('status').textContent = 'Connecting...';

            try {
                const response = await fetch('wsock://localhost/connect');
                if (!response.ok || !response.body) throw new Error('Connection failed');

                connected = true;
                buffer = new Uint8Array(0);
                document.getElementById('status').textContent = 'Connected';
                log('Connected!', 'success');

                reader = response.body.getReader();

                while (connected) {
                    const { done, value } = await reader.read();
                    if (done) { log('Connection closed by server.', 'info'); break; }

                    // Append to buffer
                    const newBuf = new Uint8Array(buffer.length + value.length);
                    newBuf.set(buffer);
                    newBuf.set(value, buffer.length);
                    buffer = newBuf;

                    // Process frames
                    while (true) {
                        const frame = decodeFrame(buffer);
                        if (!frame) break;
                        handleFrame(frame);
                        buffer = buffer.slice(frame.totalLength);
                    }
                }
            } catch (e) {
                log(`Error: ${e.message}`, 'error');
            }

            connected = false;
            document.getElementById('status').textContent = 'Disconnected';
        }

        function handleFrame(frame) {
            switch (frame.opcode) {
                case OPCODE.TEXT:
                case OPCODE.BINARY:
                    log(`Received: ${new TextDecoder().decode(frame.payload)}`, 'success');
                    stats.received++;
                    break;
                case OPCODE.PING:
                    log(`PING: ${new TextDecoder().decode(frame.payload)}`, 'ping');
                    stats.pings++;
                    sendFrame(OPCODE.PONG, frame.payload); // Echo payload in PONG
                    stats.pongs++;
                    log(`PONG sent`, 'ping');
                    break;
                case OPCODE.PONG:
                    log(`PONG: ${new TextDecoder().decode(frame.payload)}`, 'ping');
                    break;
                case OPCODE.CLOSE:
                    log('CLOSE received', 'info');
                    disconnect();
                    break;
            }
            updateStats();
        }

        async function sendMessage() {
            if (!connected) { log('Not connected!', 'error'); return; }
            const input = document.getElementById('message');
            const text = input.value.trim();
            if (!text) return;
            await sendFrame(OPCODE.TEXT, text);
            log(`Sent: ${text}`, 'info');
            stats.sent++;
            updateStats();
            input.value = '';
        }

        function disconnect() {
            connected = false;
            if (reader) { reader.cancel().catch(() => {}); reader = null; }
            sendFrame(OPCODE.CLOSE, new Uint8Array([0x03, 0xE8])).catch(() => {});
        }

        log('Ready. Click Connect to start.', 'info');

        // Auto-connect in test mode (check if path contains "test")
        if (window.location.pathname.includes('test')) {
            log('Test mode: auto-connecting...', 'info');
            setTimeout(connect, 500);
        }
    </script>
</body>
</html>
)html";

coco::stray start(saucer::application *app)
{
    g_app = app;

    saucer::webview::register_scheme("app");
    saucer::webview::register_scheme("wsock");

    auto window  = saucer::window::create(app).value();
    auto webview = saucer::smartview::create({.window = window});

    window->set_title("WebSocket over Custom Scheme");
    window->set_size({800, 600});

    webview->on<saucer::webview::event::dom_ready>([&]() {
        std::fprintf(stderr, "[SAUCER] DOM ready\n");
        g_dom_ready = true;
    });

    // Binary message handler for client → server frames (replaces POST to /send)
    webview->on<saucer::webview::event::binary_message>(
        [](std::span<const std::uint8_t> data) {
            auto result = WsFrame::decode(data.data(), data.size());
            if (result)
            {
                g_from_client.push(std::move(result->first));
            }
            return saucer::status::handled;
        });

    webview->handle_stream_scheme("wsock",
        [](saucer::scheme::request req, saucer::scheme::stream_writer writer)
        {
            if (req.url().path() == "/connect")
            {
                std::fprintf(stderr, "[WS] /connect request\n");
                g_running = true;
                g_from_client.reset();
                g_pings_sent = 0;
                g_pongs_received = 0;
                std::thread(websocket_server_thread, std::move(writer)).detach();
            }
            else
            {
                writer.reject(saucer::scheme::error::not_found);
            }
        });

    webview->handle_scheme("app", [](const saucer::scheme::request &req) {
        return saucer::scheme::response{
            .data   = saucer::stash::view_str(html_template),
            .mime   = "text/html",
            .status = 200,
        };
    });

    auto path = g_test_mode ? "/test.html" : "/index.html";
    webview->set_url(saucer::url::make({.scheme = "app", .host = "localhost", .path = path}));

    if (!g_test_mode)
        webview->set_dev_tools(true);

    window->show();

    co_await app->finish();
}

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++)
    {
        if (std::string_view(argv[i]) == "--test")
        {
            g_test_mode = true;
            std::fprintf(stderr, "[TEST] Running in test mode\n");
            break;
        }
    }

    auto result = saucer::application::create({.id = "websocket_scheme"})->run(start);

    if (g_test_mode)
    {
        std::fprintf(stderr, "[TEST] Pings: %d, Pongs: %d, Result: %s\n",
            g_pings_sent.load(), g_pongs_received.load(),
            g_test_result == 0 ? "SUCCESS" : "FAILED");
        return g_test_result.load();
    }

    return result;
}

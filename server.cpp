// server.cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#pragma comment(lib, "ws2_32.lib")

const int PORT = 5000;
const int CLIENT_PORT = 4444;

std::vector<BYTE> latestFrame;
std::mutex frameMutex;

std::string base64_encode(const BYTE* buf, size_t len) {
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t val = (uint32_t)buf[i] << 16;
        if (i + 1 < len) val |= (uint32_t)buf[i+1] << 8;
        if (i + 2 < len) val |= (uint32_t)buf[i+2];
        out.push_back(b64[(val >> 18) & 0x3F]);
        out.push_back(b64[(val >> 12) & 0x3F]);
        out.push_back((i + 1 < len) ? b64[(val >> 6) & 0x3F] : '=');
        out.push_back((i + 2 < len) ? b64[val & 0x3F] : '=');
    }
    return out;
}

std::string html = R"(
<!DOCTYPE html>
<html><head><title>Nyx</title>
<style>body{background:#000;color:#0f0;font-family:monospace;}</style>
</head><body>
<h1>Nyx Live</h1>
<div id="s"></div>
<script>
setInterval(()=>{fetch('/data').then(r=>r.text()).then(d=>{document.getElementById('s').innerHTML = '<img src="data:image/jpeg;base64,'+d+'">';});}, 600);
</script>
</body></html>
)";

void HandleClient(SOCKET client) {
    std::cout << "[CLIENT] New screen client connected!" << std::endl;
    while (true) {
        int len;
        if (recv(client, (char*)&len, 4, 0) <= 0) break;
        if (len <= 0 || len > 10 * 1024 * 1024) break;

        std::vector<BYTE> buf(len);
        int total = 0;
        while (total < len) {
            int got = recv(client, (char*)buf.data() + total, len - total, 0);
            if (got <= 0) break;
            total += got;
        }
        if (total == len) {
            std::lock_guard<std::mutex> lock(frameMutex);
            latestFrame = std::move(buf);
        }
    }
    std::cout << "[CLIENT] Screen client disconnected." << std::endl;
    closesocket(client);
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    // ---------- Screen receiver thread ----------
    std::thread([](){
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) {
            std::cerr << "[ERROR] Could not create screen socket!" << std::endl;
            return;
        }
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(CLIENT_PORT);
        a.sin_addr.s_addr = INADDR_ANY;
        if (bind(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
            std::cerr << "[ERROR] Could not bind screen port " << CLIENT_PORT 
                      << " (error " << WSAGetLastError() << "). Try running as Administrator." << std::endl;
            closesocket(s);
            return;
        }
        if (listen(s, 5) == SOCKET_ERROR) {
            std::cerr << "[ERROR] Listen failed on screen port." << std::endl;
            closesocket(s);
            return;
        }
        std::cout << "[OK] Listening for screen client on port " << CLIENT_PORT << std::endl;
        while (true) {
            SOCKET c = accept(s, NULL, NULL);
            if (c != INVALID_SOCKET)
                std::thread(HandleClient, c).detach();
        }
    }).detach();

    // ---------- HTTP server ----------
    SOCKET http = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(http, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "[ERROR] Could not bind HTTP port " << PORT << std::endl;
        closesocket(http);
        WSACleanup();
        return 1;
    }
    listen(http, 10);

    std::thread([](){
        MessageBoxA(0, "Nyx Server running on http://127.0.0.1:5000\nhttp://192.168.0.104:5000", "Nyx", 0);
    }).detach();

    std::cout << "[OK] HTTP server listening on port " << PORT << std::endl;

    while (true) {
        SOCKET conn = accept(http, NULL, NULL);
        if (conn == INVALID_SOCKET) continue;

        char buf[4096] = {};
        std::string request;
        int bytes;
        do {
            bytes = recv(conn, buf, sizeof(buf) - 1, 0);
            if (bytes > 0) {
                buf[bytes] = '\0';
                request += buf;
            }
        } while (bytes > 0 && request.find("\r\n\r\n") == std::string::npos);

        std::string response;
        if (request.substr(0, 5) == "GET /") {
            size_t pathEnd = request.find(' ', 4);
            std::string path = request.substr(4, pathEnd - 4);
            if (path == "/data") {
                std::string base64;
                {
                    std::lock_guard<std::mutex> lock(frameMutex);
                    if (!latestFrame.empty()) {
                        base64 = base64_encode(latestFrame.data(), latestFrame.size());
                    } else {
                        // Tiny 1x1 black JPEG
                        static const std::vector<BYTE> emptyJpeg = {
                            0xFF,0xD8,0xFF,0xE0,0x00,0x10,0x4A,0x46,0x49,0x46,0x00,0x01,0x01,0x00,0x00,0x01,
                            0x00,0x01,0x00,0x00,0xFF,0xDB,0x00,0x43,0x00,0x08,0x06,0x06,0x07,0x06,0x05,0x08,
                            0x07,0x07,0x07,0x09,0x09,0x08,0x0A,0x0C,0x14,0x0D,0x0C,0x0B,0x0B,0x0C,0x19,0x12,
                            0x13,0x0F,0x14,0x1D,0x1A,0x1F,0x1E,0x1D,0x1A,0x1C,0x1C,0x20,0x24,0x2E,0x27,0x20,
                            0x22,0x2C,0x23,0x1C,0x1C,0x28,0x37,0x29,0x2C,0x30,0x31,0x34,0x34,0x34,0x1F,0x27,
                            0x39,0x3D,0x38,0x32,0x3C,0x2E,0x33,0x34,0x32,0xFF,0xC0,0x00,0x0B,0x08,0x00,0x01,
                            0x00,0x01,0x01,0x01,0x11,0x00,0xFF,0xC4,0x00,0x1F,0x00,0x00,0x01,0x05,0x01,0x01,
                            0x01,0x01,0x01,0x01,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x01,0x02,0x03,0x04,
                            0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0xFF,0xC4,0x00,0xB5,0x10,0x00,0x02,0x01,0x03,
                            0x03,0x02,0x04,0x03,0x05,0x05,0x04,0x04,0x00,0x00,0x01,0x7D,0x01,0x02,0x03,0x00,
                            0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,
                            0x91,0xA1,0x08,0x23,0x42,0xB1,0xC1,0x15,0x52,0xD1,0xF0,0x24,0x33,0x62,0x72,0x82,0x09,
                            0x0A,0x16,0x17,0x18,0x19,0x1A,0x25,0x26,0x27,0x28,0x29,0x2A,0x34,0x35,0x36,0x37,0x38,
                            0x39,0x3A,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4A,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
                            0x5A,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6A,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7A,
                            0x83,0x84,0x85,0x86,0x87,0x88,0x89,0x8A,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9A,
                            0xA2,0xA3,0xA4,0xA5,0xA6,0xA7,0xA8,0xA9,0xAA,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7,0xB8,0xB9,
                            0xBA,0xC2,0xC3,0xC4,0xC5,0xC6,0xC7,0xC8,0xC9,0xCA,0xD2,0xD3,0xD4,0xD5,0xD6,0xD7,0xD8,
                            0xD9,0xDA,0xE1,0xE2,0xE3,0xE4,0xE5,0xE6,0xE7,0xE8,0xE9,0xEA,0xF1,0xF2,0xF3,0xF4,0xF5,
                            0xF6,0xF7,0xF8,0xF9,0xFA,0xFF,0xDA,0x00,0x0C,0x03,0x01,0x00,0x02,0x11,0x03,0x11,0x00,
                            0x3F,0x00,0xA2,0xE8,0x83,0xE8,0x3F,0x2A,0x3A,0x20,0xFA,0x0F,0xCA,0xBF,0xFF,0xD9
                        };
                        base64 = base64_encode(emptyJpeg.data(), emptyJpeg.size());
                    }
                }
                std::string body = base64;
                response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: " + std::to_string(body.size()) + "\r\n"
                    "Connection: close\r\n"
                    "\r\n" + body;
            } else {
                response =
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html\r\n"
                    "Content-Length: " + std::to_string(html.size()) + "\r\n"
                    "Connection: close\r\n"
                    "\r\n" + html;
            }
        } else {
            std::string body = "Not Found";
            response =
                "HTTP/1.1 404 Not Found\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n"
                "Connection: close\r\n"
                "\r\n" + body;
        }

        send(conn, response.c_str(), response.size(), 0);
        closesocket(conn);
    }

    WSACleanup();
    return 0;
}
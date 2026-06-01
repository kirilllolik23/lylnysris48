// server.cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iostream>
#include <string>
#include <vector>
#include <thread>
#pragma comment(lib, "ws2_32.lib")

const int PORT = 5000;
const int CLIENT_PORT = 4444;
const std::string BIND_IP = "192.168.0.104";  // your private IP

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
    while (true) {
        int len;
        int recvResult = recv(client, (char*)&len, 4, 0);
        if (recvResult <= 0) break;
        std::vector<char> buf(len);
        int total = 0;
        while (total < len) {
            int got = recv(client, buf.data() + total, len - total, 0);
            if (got <= 0) break;
            total += got;
        }
        // (future: handle the screen buffer)
    }
    closesocket(client);
}

int main() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        MessageBoxA(0, "WSAStartup failed", "Error", 0);
        return 1;
    }

    // ---------- Client listener thread ----------
    std::thread([](){
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) return;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(CLIENT_PORT);
        a.sin_addr.s_addr = INADDR_ANY;
        if (bind(s, (sockaddr*)&a, sizeof(a)) == SOCKET_ERROR) {
            closesocket(s);
            return;
        }
        listen(s, 5);
        while (true) {
            SOCKET c = accept(s, NULL, NULL);
            if (c != INVALID_SOCKET)
                std::thread(HandleClient, c).detach();
        }
    }).detach();

    // ---------- HTTP server ----------
    SOCKET http = socket(AF_INET, SOCK_STREAM, 0);
    if (http == INVALID_SOCKET) {
        MessageBoxA(0, "Socket creation failed", "Error", 0);
        WSACleanup();
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    // Convert the IP string to binary
    if (inet_pton(AF_INET, BIND_IP.c_str(), &addr.sin_addr) != 1) {
        MessageBoxA(0, "Invalid IP address", "Error", 0);
        closesocket(http);
        WSACleanup();
        return 1;
    }

    if (bind(http, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        MessageBoxA(0, "Bind failed. Try running as Administrator.", "Error", 0);
        closesocket(http);
        WSACleanup();
        return 1;
    }
    if (listen(http, 10) == SOCKET_ERROR) {
        MessageBoxA(0, "Listen failed", "Error", 0);
        closesocket(http);
        WSACleanup();
        return 1;
    }

    // Show a message box WITHOUT blocking the server
    std::thread([](){
        MessageBoxA(0,
            "Nyx Server running on http://192.168.0.104:5000\n"
            "Make sure your firewall allows port 5000!",
            "Nyx", 0);
    }).detach();

    std::cout << "Server listening on " << BIND_IP << ":" << PORT << std::endl;

    while (true) {
        SOCKET conn = accept(http, NULL, NULL);
        if (conn == INVALID_SOCKET) {
            std::cerr << "Accept failed: " << WSAGetLastError() << std::endl;
            continue;
        }

        // Build the response correctly
        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: " + std::to_string(html.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + html;

        send(conn, response.c_str(), response.size(), 0);
        closesocket(conn);
    }

    WSACleanup();
    return 0;
}
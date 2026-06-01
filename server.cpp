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
    }
    closesocket(client);
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    // Client screen‑capture listener
    std::thread([](){
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(CLIENT_PORT);
        a.sin_addr.s_addr = INADDR_ANY;
        bind(s, (sockaddr*)&a, sizeof(a));
        listen(s, 5);
        while (true) {
            SOCKET c = accept(s, NULL, NULL);
            if (c != INVALID_SOCKET)
                std::thread(HandleClient, c).detach();
        }
    }).detach();

    // HTTP server
    SOCKET http = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;          // listen on all interfaces
    bind(http, (sockaddr*)&addr, sizeof(addr));
    listen(http, 10);

    std::thread([](){
        MessageBoxA(0, "Nyx Server running on http://127.0.0.1:5000\nhttp://192.168.0.104:5000", "Nyx", 0);
    }).detach();

    std::cout << "Server listening on port " << PORT << std::endl;

    while (true) {
        SOCKET conn = accept(http, NULL, NULL);
        if (conn == INVALID_SOCKET) continue;

        // Read the browser's request
        char buf[4096];
        std::string request;
        int bytes;
        do {
            bytes = recv(conn, buf, sizeof(buf) - 1, 0);
            if (bytes > 0) {
                buf[bytes] = '\0';
                request += buf;
            }
        } while (bytes > 0 && request.find("\r\n\r\n") == std::string::npos);

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
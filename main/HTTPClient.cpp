#include "HTTPClient.h"

#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

#include <cstring>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <fstream>
#include <chrono>

// ===========================================
// Constructor / Destructor
// ===========================================

HTTPClient::HTTPClient() : sockfd(-1), connected(false) {}

HTTPClient::~HTTPClient() {
    closeConnection();
}

// ===========================================
// URL Parsing
// ===========================================

URL HTTPClient::parseURL(const std::string& url) {
    URL result;
    result.full = url;

    size_t pos = 0;

    // protocol
    size_t protocolEnd = url.find("://");
    if (protocolEnd != std::string::npos) {
        result.protocol = url.substr(0, protocolEnd);
        pos = protocolEnd + 3;
    } else {
        result.protocol = "http";
        pos = 0;
    }

    // host + port
    size_t pathStart = url.find('/', pos);
    size_t hostEnd = (pathStart == std::string::npos ? url.size() : pathStart);
    std::string hostPart = url.substr(pos, hostEnd - pos);

    size_t portPos = hostPart.find(':');
    if (portPos != std::string::npos) {
        result.host = hostPart.substr(0, portPos);
        result.port = std::stoi(hostPart.substr(portPos + 1));
    } else {
        result.host = hostPart;
        result.port = (result.protocol == "https") ? 443 : 80;
    }

    // path
    if (pathStart != std::string::npos)
        result.path = url.substr(pathStart);
    else
        result.path = "/";

    // query
    size_t queryPos = result.path.find('?');
    if (queryPos != std::string::npos) {
        result.query = result.path.substr(queryPos + 1);
        result.path = result.path.substr(0, queryPos);
    }

    return result;
}

// ===========================================
// Socket Connection
// ===========================================

bool HTTPClient::connectToServer(const std::string& host, int port) {
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        std::cerr << "Error: cannot create socket\n";
        return false;
    }

    // timeout
    struct timeval timeout;
    timeout.tv_sec = TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    // dns lookup
    struct hostent* server = gethostbyname(host.c_str());
    if (!server) {
        std::cerr << "Error: cannot resolve host " << host << "\n";
        close(sockfd);
        sockfd = -1;
        return false;
    }

    // server address
    struct sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port);
    memcpy(&serverAddr.sin_addr.s_addr, server->h_addr, server->h_length);

    // connect
    if (connect(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
        std::cerr << "Error: cannot connect to " << host << ":" << port << "\n";
        close(sockfd);
        sockfd = -1;
        return false;
    }

    connected = true;
    return true;
}

void HTTPClient::closeConnection() {
    if (sockfd >= 0) {
        close(sockfd);
        sockfd = -1;
    }
    connected = false;
}

// ===========================================
// Send / Receive
// ===========================================

bool HTTPClient::sendRequest(const std::string& request) {
    if (!connected || sockfd < 0)
        return false;

    size_t total = 0;
    size_t len = request.size();

    while (total < len) {
        ssize_t sent = send(sockfd, request.c_str() + total, len - total, 0);
        if (sent < 0) {
            std::cerr << "Error: send failed\n";
            return false;
        }
        total += sent;
    }
    return true;
}

std::string HTTPClient::receiveData() {
    std::string result;
    char buffer[BUFFER_SIZE];

    while (true) {
        ssize_t received = recv(sockfd, buffer, BUFFER_SIZE, 0);
        if (received < 0) {
            std::cerr << "Error: recv failed\n";
            break;
        }
        if (received == 0)
            break;
        result.append(buffer, received);
    }
    return result;
}

// ===========================================
// Helper Functions
// ===========================================

std::string HTTPClient::toLowerCase(const std::string& s) {
    std::string r = s;
    std::transform(r.begin(), r.end(), r.begin(), ::tolower);
    return r;
}

// ===========================================
// HTTP Response Parsing
// ===========================================

HTTPResponse HTTPClient::parseResponse(const std::string& raw) {
    HTTPResponse resp;

    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        resp.statusCode = -1;
        return resp;
    }

    std::string header = raw.substr(0, headerEnd);
    std::string body = raw.substr(headerEnd + 4);

    // status line
    std::istringstream ss(header);
    std::string statusLine;
    std::getline(ss, statusLine);

    if (!statusLine.empty() && statusLine.back() == '\r')
        statusLine.pop_back();

    std::istringstream sl(statusLine);
    std::string httpVersion;
    sl >> httpVersion >> resp.statusCode;
    std::getline(sl, resp.statusMessage);
    if (!resp.statusMessage.empty() && resp.statusMessage[0] == ' ')
        resp.statusMessage.erase(0, 1);

    // headers
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();

        size_t pos = line.find(':');
        if (pos != std::string::npos) {
            std::string key = toLowerCase(line.substr(0, pos));
            std::string val = line.substr(pos + 1);

            // trim
            val.erase(0, val.find_first_not_of(" \t"));
            val.erase(val.find_last_not_of(" \t") + 1);

            resp.headers[key] = val;
        }
    }

    resp.body.assign(body.begin(), body.end());
    resp.bodyText = body;

    return resp;
}

// ===========================================
// GET / HEAD
// ===========================================

HTTPResponse HTTPClient::get(const std::string& url) {
    URL parsed = parseURL(url);

    if (!connectToServer(parsed.host, parsed.port)) {
        HTTPResponse r;
        r.statusCode = -1;
        return r;
    }

    std::ostringstream req;
    req << "GET " << parsed.path << " HTTP/1.1\r\n"
        << "Host: " << parsed.host << "\r\n"
        << "Connection: close\r\n"
        << "User-Agent: CustomClient/1.0\r\n\r\n";

    if (!sendRequest(req.str())) {
        closeConnection();
        HTTPResponse r;
        r.statusCode = -1;
        return r;
    }

    std::string raw = receiveData();
    closeConnection();
    return parseResponse(raw);
}

HTTPResponse HTTPClient::head(const std::string& url) {
    URL parsed = parseURL(url);

    if (!connectToServer(parsed.host, parsed.port)) {
        HTTPResponse r;
        r.statusCode = -1;
        return r;
    }

    std::ostringstream req;
    req << "HEAD " << parsed.path << " HTTP/1.1\r\n"
        << "Host: " << parsed.host << "\r\n"
        << "Connection: close\r\n"
        << "User-Agent: CustomClient/1.0\r\n\r\n";

    if (!sendRequest(req.str())) {
        closeConnection();
        HTTPResponse r;
        r.statusCode = -1;
        return r;
    }

    std::string raw = receiveData();
    closeConnection();
    return parseResponse(raw);
}

// ===========================================
// Download File Wrapper
// ===========================================

bool HTTPClient::downloadFile(const std::string& url,
                              const std::string& savePath,
                              void (*progress)(size_t, size_t))
{
    HTTPResponse resp = get(url);

    if (resp.statusCode != 200) {
        std::cerr << "HTTP " << resp.statusCode << ": " << resp.statusMessage << "\n";
        return false;
    }

    std::ofstream out(savePath, std::ios::binary);
    if (!out) {
        std::cerr << "Error: cannot open " << savePath << "\n";
        return false;
    }

    size_t total = resp.body.size();
    size_t written = 0;

    const size_t CHUNK_SIZE = 4096;
    auto start = std::chrono::steady_clock::now();

    while (written < total) {
        size_t n = std::min(CHUNK_SIZE, total - written);
        out.write(resp.body.data() + written, n);
        written += n;

        // 計算速度與 ETA
        auto now = std::chrono::steady_clock::now();
        double seconds = std::chrono::duration<double>(now - start).count();
        if (seconds <= 0.0) seconds = 0.001;   // 避免除以 0
        double speed = written / 1024.0 / seconds;       // KB/s
        double eta   = (total > written)
                       ? (total - written) / 1024.0 / speed
                       : 0.0;                              // seconds

        // 顯示進度（callback）
        if (progress) {
            progress(written, total);
        }

        std::cout << "  Speed: " << speed << " KB/s, "
                  << "ETA: " << eta << "s     \r" << std::flush;
    }

    std::cout << std::endl;
    out.close();
    return true;
}

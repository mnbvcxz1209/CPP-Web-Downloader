#include <iostream>
#include <string>
#include <queue>
#include <set>
#include <map>
#include <vector>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstring>
#include <sys/stat.h>
#include <sys/types.h>
#include <chrono>
#include <iomanip>
#include <algorithm>
#include <regex>
#include <signal.h>
#include <errno.h>

using namespace std;

// 全域變數用於中斷處理
bool g_interrupted = false;

// 信號處理函數
void signalHandler(int signum) {
    g_interrupted = true;
    cout << "\n\n[中斷] 按 Enter 停止下載..." << endl;
}

// URL 結構
struct URL {
    string protocol;
    string host;
    string path;
    int port;
    int depth;
    
    URL() : port(80), depth(0) {}
};

// 下載統計
struct Stats {
    int totalFiles;
    int htmlFiles;
    int imageFiles;
    int otherFiles;
    long long totalBytes;
    int estimatedTotalFiles;  // 預估總檔案數
    chrono::steady_clock::time_point startTime;
    
    Stats() : totalFiles(0), htmlFiles(0), imageFiles(0), otherFiles(0), 
              totalBytes(0), estimatedTotalFiles(0) {
        startTime = chrono::steady_clock::now();
    }
    
    double getElapsedSeconds() {
        auto now = chrono::steady_clock::now();
        return chrono::duration<double>(now - startTime).count();
    }
    
    //  計算預估剩餘時間
    double getEstimatedRemainingSeconds(int queueSize) {
        if (totalFiles == 0) return 0.0;
        
        double elapsed = getElapsedSeconds();
        double avgTimePerFile = elapsed / totalFiles;
        
        // 預估剩餘檔案數 = 佇列中的檔案
        return avgTimePerFile * queueSize;
    }
    
    //  計算完成百分比
    double getProgressPercentage(int queueSize) {
        int processed = totalFiles;
        int total = processed + queueSize;
        
        if (total == 0) return 0.0;
        return (double)processed / total * 100.0;
    }
};

// 下載器類別
class WebDownloader {
private:
    string baseURL;
    string outputDir;
    int maxDepth;
    bool downloadExternal;
    bool downloadDocuments;
    string baseHost;
    
    // 進階功能
    set<string> fileTypeFilter;     // 檔案類型過濾（空集合表示下載所有類型）
    long long minFileSize;           // 最小檔案大小（bytes，0 表示無限制）
    long long maxFileSize;           // 最大檔案大小（bytes，0 表示無限制）
    
    queue<pair<string, int>> urlQueue;  // URL 和深度
    set<string> visited;
    map<string, bool> downloadStatus;
    Stats stats;
    
    // 解析 URL
    URL parseURL(const string& urlStr) {
        URL url;
        size_t pos = 0;
        
        // 檢查 URL 格式是否正確
        if (urlStr.find("http:/") != string::npos && urlStr.find("http://") == string::npos) {
            // 發現錯誤格式
            cout << "[錯誤] URL 格式不正確: " << urlStr << endl;
            return url;  // 返回空 URL
        }
        
        // 解析協議
        size_t protocolEnd = urlStr.find("://");
        if (protocolEnd != string::npos) {
            url.protocol = urlStr.substr(0, protocolEnd);
            pos = protocolEnd + 3;
        } else {
            
            url.protocol = "http";
            pos = 0;
        }
        
        // 解析主機和路徑
        size_t pathStart = urlStr.find('/', pos);//behind "/"->pathSTART;before "/"->host
        if (pathStart != string::npos) {
            string hostPort = urlStr.substr(pos, pathStart - pos);
            url.path = urlStr.substr(pathStart);
            
            // 解析端口if dont port are using,default be 80
            size_t colonPos = hostPort.find(':');
            if (colonPos != string::npos) {
                url.host = hostPort.substr(0, colonPos);
                url.port = stoi(hostPort.substr(colonPos + 1));
            } else {
                url.host = hostPort;
                url.port = 80;
            }
        } else {
            url.host = urlStr.substr(pos);
            url.path = "/";
            url.port = 80;
        }
        
        return url;
    }
    
    // 建立 HTTP GET 請求
    string buildHTTPRequest(const URL& url) {
        stringstream ss;
        ss << "GET " << url.path << " HTTP/1.1\r\n";
        ss << "Host: " << url.host << "\r\n";
        ss << "User-Agent: WebDownloader/1.0\r\n";
        ss << "Accept: */*\r\n";
        ss << "Connection: close\r\n";
        ss << "\r\n";
        return ss.str();
    }
    
    // 建立 Socket 連接
    int connectToHost(const string& host, int port) {
        struct hostent* server = gethostbyname(host.c_str());
        if (server == NULL) {
            return -1;
        }
        
        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            return -1;
        }
        
        // 設定超時
        struct timeval timeout;
        timeout.tv_sec = 10;
        timeout.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        
        struct sockaddr_in serverAddr;
        memset(&serverAddr, 0, sizeof(serverAddr));
        serverAddr.sin_family = AF_INET;
        memcpy(&serverAddr.sin_addr.s_addr, server->h_addr, server->h_length);
        serverAddr.sin_port = htons(port);
        
        if (connect(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) < 0) {
            close(sockfd);
            return -1;
        }
        
        return sockfd;
    }
    
    // 發送 HTTP 請求並接收回應
    string sendHTTPRequest(int sockfd, const string& request) {
        send(sockfd, request.c_str(), request.length(), 0);
        
        string response;
        char buffer[4096];
        int bytesRead;
        
        while ((bytesRead = recv(sockfd, buffer, sizeof(buffer) - 1, 0)) > 0) {
            response.append(buffer, bytesRead);
        }
        
        return response;
    }
    
    // 解析 HTTP 回應
    pair<map<string, string>, string> parseHTTPResponse(const string& response) {
        map<string, string> headers;
        string body;
        
        size_t headerEnd = response.find("\r\n\r\n");
        if (headerEnd == string::npos) {
            headerEnd = response.find("\n\n");
            if (headerEnd == string::npos) {
                return {headers, response};
            }
            body = response.substr(headerEnd + 2);
        } else {
            body = response.substr(headerEnd + 4);
        }
        
        string headerSection = response.substr(0, headerEnd);
        
        stringstream ss(headerSection);
        string line;
        
        // 跳過狀態行
        getline(ss, line);
        
        // 解析標頭
        while (getline(ss, line)) {
            if (line.empty() || line == "\r") break;
            
            // 移除 \r
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            
            size_t colonPos = line.find(':');
            if (colonPos != string::npos) {
                string key = line.substr(0, colonPos);
                string value = line.substr(colonPos + 1);
                
                // 移除前後空白
                value.erase(0, value.find_first_not_of(" \t\r\n"));
                value.erase(value.find_last_not_of(" \t\r\n") + 1);
                
                headers[key] = value;
            }
        }
        
        return {headers, body};
    }
    
    // 從 HTML 提取連結
    vector<string> extractLinks(const string& html, const string& baseURL) {
        vector<string> links;
        set<string> uniqueLinks;  // 避免重複
        
        
        
        // 1. <a href="..."> - 網頁連結
        regex hrefPattern(R"(<a\s+[^>]*href\s*=\s*["']([^"']+)["'])", regex::icase);
        // 2. <img src="..."> - 圖片
        regex imgPattern(R"(<img\s+[^>]*src\s*=\s*["']([^"']+)["'])", regex::icase);
        // 3. <script src="..."> - JavaScript
        regex scriptPattern(R"(<script\s+[^>]*src\s*=\s*["']([^"']+)["'])", regex::icase);
        // 4. <link href="..."> - CSS 等
        regex linkPattern(R"(<link\s+[^>]*href\s*=\s*["']([^"']+)["'])", regex::icase);
        
        vector<pair<regex, string>> patterns = {
            {hrefPattern, "href"},
            {imgPattern, "img"},
            {scriptPattern, "script"},
            {linkPattern, "link"}
        };
        
        for (const auto& [pattern, type] : patterns) {
            sregex_iterator iter(html.begin(), html.end(), pattern);
            sregex_iterator end;
            
            while (iter != end) {
                string link = (*iter)[1];
                
                // 清理連結（移除前後空白）
                link.erase(0, link.find_first_not_of(" \t\r\n"));
                link.erase(link.find_last_not_of(" \t\r\n") + 1);
                
                // 驗證提取的連結是否合法
                bool isValid = true;
                
                // 檢查是否包含不合法的字符
                if (link.find("http:/") != string::npos && link.find("http://") == string::npos) {
                    // 發現 "http:/" 但不是 "http://"
                    cout << "  [警告] 忽略不合法連結: " << link << endl;
                    isValid = false;
                }
                
                if (isValid) {
                    // 轉換相對路徑為絕對路徑
                    string absoluteLink = resolveRelativeURL(link, baseURL);
                    
                    if (!absoluteLink.empty() && uniqueLinks.find(absoluteLink) == uniqueLinks.end()) {
                        uniqueLinks.insert(absoluteLink);
                        links.push_back(absoluteLink);
                    }
                }
                
                ++iter;
            }
        }
        
        return links;
    }
    
    // 解析相對 URL（避免巢狀路徑）
    string resolveRelativeURL(const string& link, const string& base) {
        // 忽略不需要的連結
        if (link.empty() || link[0] == '#' || 
            link.find("javascript:") == 0 || link.find("mailto:") == 0) {
            return "";
        }
        
        if (link.find("http://") == 0 || link.find("https://") == 0) {
            return link;  
        }
        
        
        if (link.find("//") == 0) {
            URL baseUrl = parseURL(base);
            return baseUrl.protocol + ":" + link;
        }
        
        URL baseUrl = parseURL(base);
        
        // 處理從根目錄開始的絕對路徑 
        if (link[0] == '/') {
            string result = baseUrl.protocol + "://" + baseUrl.host;
            if (baseUrl.port != 80 && baseUrl.port != 443) {
                result += ":" + to_string(baseUrl.port);
            }
            result += link;
            return result;
        }
        
        // 處理相對路徑
        string basePath = baseUrl.path;
        
       
        size_t lastSlash = basePath.find_last_of('/');
        size_t lastDot = basePath.find_last_of('.');
        
        // 判斷是否為檔案（
        if (lastDot != string::npos && lastSlash != string::npos && lastDot > lastSlash) {
            // 這是一個檔案
            basePath = basePath.substr(0, lastSlash + 1);
        } else {
            // 這是一個目錄，確保以 / 結尾
            if (!basePath.empty() && basePath.back() != '/') {
                basePath += '/';
            }
        }
        
        // 移除 ./ 前綴
        string cleanLink = link;
        if (cleanLink.find("./") == 0) {
            cleanLink = cleanLink.substr(2);
        }
        
        // 組合路徑
        string fullPath = basePath + cleanLink;
        
        // 處理 ../ 相對路徑
        while (fullPath.find("/../") != string::npos) {
            size_t pos = fullPath.find("/../");
            if (pos == 0) {
                // 無法再往上
                fullPath = fullPath.substr(3);
                break;
            }
            
            size_t prevSlash = fullPath.rfind('/', pos - 1);
            if (prevSlash != string::npos) {
                fullPath = fullPath.substr(0, prevSlash) + fullPath.substr(pos + 3);
            } else {
                break;
            }
        }
        
        // 組合完整 URL
        string result = baseUrl.protocol + "://" + baseUrl.host;
        if (baseUrl.port != 80 && baseUrl.port != 443) {
            result += ":" + to_string(baseUrl.port);
        }
        result += fullPath;
        
        return result;
    }
    
    // 修改 HTML 中的連結為本地路徑（支援跨域名）
    string convertLinksToLocal(const string& html, const string& baseURL) {
        string result = html;
        
        cout << "\n[轉換連結] 修改 HTML 以支援離線瀏覽" << endl;
        
        URL currentPage = parseURL(baseURL);
        string baseDir = outputDir + "/" + currentPage.host;
        if (currentPage.port != 80 && currentPage.port != 443) {
            baseDir += "_" + to_string(currentPage.port);
        }
        
        // HTML 檔案的完整路徑
        string htmlFilePath = getLocalPath(baseURL);
        
        cout << "  當前頁面：" << htmlFilePath << endl;
        
        int convertedCount = 0;
        
        // 使用正則表達式找到所有 src 和 href 屬性
        regex srcPattern(R"((src|href)\s*=\s*["']([^"']+)["'])");
        sregex_iterator iter(html.begin(), html.end(), srcPattern);
        sregex_iterator end;
        
        map<string, string> replacements;  // 原始 → 本地路徑
        
        while (iter != end) {
            string fullMatch = (*iter)[0];
            string attrName = (*iter)[1];  // "src" 或 "href"
            string url = (*iter)[2];       // URL 值
            
            // 跳過錨點和特殊協議
            if (url.empty() || url[0] == '#' || 
                url.find("javascript:") == 0 || url.find("mailto:") == 0) {
                ++iter;
                continue;
            }
            
            // 解析 URL 為絕對路徑
            string absoluteURL = resolveRelativeURL(url, baseURL);
            
            if (!absoluteURL.empty() && absoluteURL != baseURL) {
                // 取得本地路徑
                string localPath = getLocalPath(absoluteURL);
                
                // 計算相對路徑
                string relativePath = calculateRelativePath(htmlFilePath, localPath);
                
                // 記錄要替換的內容
                string oldValue = attrName + "=\"" + url + "\"";
                string newValue = attrName + "=\"" + relativePath + "\"";
                
                if (url != relativePath) {  // 只有當路徑改變時才替換
                    replacements[oldValue] = newValue;
                    
                    // 檢查是否為跨域名連結
                    URL targetURL = parseURL(absoluteURL);
                    bool isCrossDomain = (targetURL.host != currentPage.host);
                    
                    if (isCrossDomain) {
                        cout << "  [跨域] " << url << " → " << relativePath << endl;
                    } else {
                        cout << "  " << url << " → " << relativePath << endl;
                    }
                }
            }
            
            ++iter;
        }
        
        // 執行替換
        for (const auto& [oldVal, newVal] : replacements) {
            size_t pos = 0;
            while ((pos = result.find(oldVal, pos)) != string::npos) {
                result.replace(pos, oldVal.length(), newVal);
                pos += newVal.length();
                convertedCount++;
            }
        }
        
        cout << "[轉換完成] 共轉換 " << convertedCount << " 個連結" << endl;
        
        return result;
    }
    
    // 計算相對路徑（支援跨域名）
    string calculateRelativePath(const string& fromPath, const string& toPath) {
        // 從 fromPath（HTML 檔案）到 toPath（資源檔案）的相對路徑
        // 支援跨域名的情況
        
        // 提取兩個路徑中的主機名
        // fromPath: output/hsccl.fr.to/index.html
        // toPath:   output/hsccl.us.to/images/image.jpg
        
        size_t outputDirLen = outputDir.length();
        
        // 移除 "output/" 前綴
        string fromRelative = fromPath.substr(outputDirLen + 1);  // hsccl.fr.to/index.html
        string toRelative = toPath.substr(outputDirLen + 1);      // hsccl.us.to/images/image.jpg
        
        // 提取主機名
        size_t fromFirstSlash = fromRelative.find('/');
        size_t toFirstSlash = toRelative.find('/');
        
        string fromHost = fromRelative.substr(0, fromFirstSlash);
        string toHost = toRelative.substr(0, toFirstSlash);
        
        // 如果是同一個主機，使用簡單相對路徑
        if (fromHost == toHost) {
            // 取得 fromPath 的目錄
            size_t fromLastSlash = fromPath.find_last_of('/');
            string fromDir = (fromLastSlash != string::npos) ? fromPath.substr(0, fromLastSlash + 1) : "";
            
            // 檢查 toPath 是否以 fromDir 開頭
            if (toPath.find(fromDir) == 0) {
                // 在同一目錄或子目錄，返回相對路徑
                return toPath.substr(fromDir.length());
            }
            
            // 如果不在同一目錄樹，返回檔名
            size_t toLastSlash = toPath.find_last_of('/');
            return (toLastSlash != string::npos) ? toPath.substr(toLastSlash + 1) : toPath;
        }
        
        //  跨域名：需要返回絕對路徑
        // 例如：從 hsccl.fr.to/animals/cats.htm 到 hsccl.us.to/images/image.jpg
        // 結果：../../hsccl.us.to/images/image.jpg
        
        // 計算 fromPath 的深度
        int depth = 0;
        for (size_t i = fromFirstSlash; i < fromRelative.length(); i++) {
            if (fromRelative[i] == '/') depth++;
        }
        
        // 生成相對路徑：每一層用 "../"
        string relativePath;
        for (int i = 0; i < depth; i++) {
            relativePath += "../";
        }
        
        // 加上目標路徑
        relativePath += toRelative;
        
        return relativePath;
    }
    bool createDirectories(const string& path) {
        if (path.empty()) return true;
        
        // 使用 mkdir -p 命令
        string cmd = "mkdir -p \"" + path + "\" 2>/dev/null";
        int result = system(cmd.c_str());
        
        if (result == 0) {
            return true;
        }
        
        
        vector<string> dirs;
        string current;
        
        for (size_t i = 0; i < path.length(); i++) {
            if (path[i] == '/') {
                if (!current.empty() && current != ".") {
                    dirs.push_back(current);
                }
                current += '/';
            } else {
                current += path[i];
            }
        }
        
        // 逐層建立
        string buildPath;
        for (const auto& dir : dirs) {
            buildPath = dir;
            
            struct stat st;
            if (stat(buildPath.c_str(), &st) != 0) {
                if (mkdir(buildPath.c_str(), 0755) != 0 && errno != EEXIST) {
                    return false;
                }
            }
        }
        
        return true;
    }
    
    // 取得本地檔案路徑
    string getLocalPath(const string& url) {
        URL parsed = parseURL(url);
        
        // 基礎路徑：output/主機名/
        string basePath = outputDir + "/" + parsed.host;
        
        // 加上端口
        if (parsed.port != 80 && parsed.port != 443) {
            basePath += "_" + to_string(parsed.port);
        }
        
        // 判斷檔案類型
        string lowerPath = parsed.path;
        transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
        
        bool isImage = (lowerPath.find(".jpg") != string::npos || 
                       lowerPath.find(".jpeg") != string::npos ||
                       lowerPath.find(".png") != string::npos ||
                       lowerPath.find(".gif") != string::npos);
        
        // 所有圖片都放在 images/ 目錄
        if (isImage) {
            // 從原始路徑提取檔名
            size_t lastSlash = parsed.path.find_last_of('/');
            string filename = (lastSlash != string::npos) ? 
                             parsed.path.substr(lastSlash + 1) : parsed.path;
            
            // 生成唯一檔名以避免衝突
            string uniqueFilename = generateUniqueFilename(parsed.path, filename);
            
            //  所有圖片統一放在 images/ 目錄下
            return basePath + "/images/" + uniqueFilename;
        }
        
        // HTML 檔案：提取檔名放在根目錄
        if (lowerPath.find(".html") != string::npos || lowerPath.find(".htm") != string::npos) {
            size_t lastSlash = parsed.path.find_last_of('/');
            string filename = (lastSlash != string::npos) ? 
                             parsed.path.substr(lastSlash + 1) : parsed.path;
            
            return basePath + "/" + filename;
        }
        
        // 其他檔案：保持原始路徑
        string path = basePath + parsed.path;
        
        if (path.back() == '/') {
            path += "index.html";
        }
        
        return path;
    }
    
    // 生成唯一檔名
    string generateUniqueFilename(const string& fullPath, const string& filename) {
        // 如果檔案在根目錄或 images/ 直接目錄下，直接使用檔名
        // 計算路徑中有多少層目錄
        int slashCount = 0;
        for (char c : fullPath) {
            if (c == '/') slashCount++;
        }
        
        // /image.jpg → 1 個斜線 → 直接用 image.jpg
        if (slashCount <= 1) {
            return filename;
        }
        
        // 提取倒數第二層目錄作為前綴
        // /images/image.jpg → images_image.jpg
        // /images/animals/cats/image.jpg → cats_image.jpg
        
        size_t lastSlash = fullPath.find_last_of('/');
        if (lastSlash != string::npos && lastSlash > 0) {
            string pathPart = fullPath.substr(0, lastSlash);
            size_t secondLastSlash = pathPart.find_last_of('/');
            
            if (secondLastSlash != string::npos) {
                string dirName = pathPart.substr(secondLastSlash + 1);
                
                // 如果目錄名不為空且不是 "images"，加上前綴
                if (!dirName.empty() && dirName != "images") {
                    return dirName + "_" + filename;
                } else if (dirName == "images") {
                    // 如果是 images/ 目錄，加上 "images_" 前綴
                    return "images_" + filename;
                }
            }
        }
        
        return filename;
    }
    
    // 取得檔案副檔名
    string getFileExtension(const string& url) {
        size_t lastDot = url.find_last_of('.');
        size_t lastSlash = url.find_last_of('/');
        
        if (lastDot != string::npos && (lastSlash == string::npos || lastDot > lastSlash)) {
            string ext = url.substr(lastDot + 1);
            // 轉換為小寫
            transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            // 移除查詢參數
            size_t queryPos = ext.find('?');
            if (queryPos != string::npos) {
                ext = ext.substr(0, queryPos);
            }
            return ext;
        }
        return "";
    }
    
    // 檢查檔案類型是否符合過濾條件
    bool isFileTypeAllowed(const string& url) {
        // 如果沒有設定過濾，允許所有類型
        if (fileTypeFilter.empty()) {
            return true;
        }
        
        string ext = getFileExtension(url);
        if (ext.empty()) {
            return true;  // 沒有副檔名的檔案默認允許
        }
        
        return fileTypeFilter.find(ext) != fileTypeFilter.end();
    }
    
    //  檢查檔案大小是否符合限制
    bool isFileSizeAllowed(long long fileSize) {
        // 檢查最小大小
        if (minFileSize > 0 && fileSize < minFileSize) {
            return false;
        }
        
        // 檢查最大大小
        if (maxFileSize > 0 && fileSize > maxFileSize) {
            return false;
        }
        
        return true;
    }
    string getFileType(const string& url, const map<string, string>& headers) {
        // 先檢查 Content-Type
        if (headers.find("Content-Type") != headers.end()) {
            string contentType = headers.at("Content-Type");
            if (contentType.find("text/html") != string::npos) return "HTML";
            if (contentType.find("image/") != string::npos) return "IMAGE";
            if (contentType.find("text/css") != string::npos) return "CSS";
            if (contentType.find("javascript") != string::npos) return "JS";
            
            // 文件類型
            if (contentType.find("application/pdf") != string::npos) return "DOCUMENT";
            if (contentType.find("application/msword") != string::npos) return "DOCUMENT";
            if (contentType.find("application/vnd.ms-") != string::npos) return "DOCUMENT";
            if (contentType.find("application/vnd.openxmlformats") != string::npos) return "DOCUMENT";
        }
        
        // 根據副檔名判斷
        string lower = url;
        transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        
        // 網頁相關
        if (lower.find(".html") != string::npos || lower.find(".htm") != string::npos) return "HTML";
        if (lower.find(".css") != string::npos) return "CSS";
        if (lower.find(".js") != string::npos) return "JS";
        
        // 圖片
        if (lower.find(".png") != string::npos || lower.find(".jpg") != string::npos || 
            lower.find(".jpeg") != string::npos || lower.find(".gif") != string::npos ||
            lower.find(".svg") != string::npos || lower.find(".webp") != string::npos) return "IMAGE";
        
        //  文件類型
        if (lower.find(".pdf") != string::npos || lower.find(".doc") != string::npos ||
            lower.find(".docx") != string::npos || lower.find(".ppt") != string::npos ||
            lower.find(".pptx") != string::npos || lower.find(".xls") != string::npos ||
            lower.find(".xlsx") != string::npos || lower.find(".zip") != string::npos ||
            lower.find(".rar") != string::npos) return "DOCUMENT";
        
        return "OTHER";
    }
    
    // 儲存檔案
    bool saveFile(const string& path, const string& content) {
        // 先確保父目錄存在
        size_t lastSlash = path.find_last_of('/');
        if (lastSlash != string::npos) {
            string dirPath = path.substr(0, lastSlash);
            
            // 使用系統命令建立目錄
            string cmd = "mkdir -p \"" + dirPath + "\"";
            system(cmd.c_str());
            
            // 驗證目錄是否存在
            struct stat st;
            if (stat(dirPath.c_str(), &st) != 0) {
                cout << "[錯誤] 無法建立目錄: " << dirPath << endl;
                return false;
            }
        }
        
        // 建立並寫入檔案
        ofstream file(path, ios::binary);
        if (!file.is_open()) {
            cout << "[錯誤] 無法建立檔案: " << path << endl;
            cout << "       錯誤原因: " << strerror(errno) << endl;
            return false;
        }
        
        file.write(content.c_str(), content.length());
        file.close();
        
        // 驗證檔案是否成功寫入
        struct stat fileStat;
        if (stat(path.c_str(), &fileStat) == 0) {
            return true;
        }
        
        return false;
    }
    
    // 顯示下載進度
    void displayProgress(const string& url, long long received, long long total, double speed, const string& fileType) {
        cout << "\r[下載中] ";
        
        // 截短 URL 顯示
        string displayURL = url;
        if (displayURL.length() > 40) {
            displayURL = displayURL.substr(0, 37) + "...";
        }
        cout << displayURL << " ";
        
        // 檔案類型
        cout << "[" << fileType << "] ";
        
        if (total > 0) {
            double progress = (double)received / total * 100.0;
            cout << fixed << setprecision(1) << progress << "% ";
        }
        
        cout << "(" << received / 1024 << " KB";
        if (total > 0) {
            cout << " / " << total / 1024 << " KB";
        }
        cout << ") ";
        
        if (speed > 0) {
            cout << fixed << setprecision(1) << speed / 1024 << " KB/s";
        }
        
        cout << "        " << flush;  // 清除多餘字元
    }
    
    // 顯示統計資訊
    void displayStats() {
        int queueSize = urlQueue.size();
        double elapsed = stats.getElapsedSeconds();
        double remaining = stats.getEstimatedRemainingSeconds(queueSize);
        double progress = stats.getProgressPercentage(queueSize);
        
        cout << "\n\n========== 下載統計 ==========\n";
        
        //  進度資訊
        cout << "【進度】\n";
        cout << "  完成度: " << fixed << setprecision(1) << progress << "%\n";
        cout << "  已下載: " << stats.totalFiles << " 個檔案\n";
        cout << "  待下載: " << queueSize << " 個檔案\n";
        
        //  時間資訊
        cout << "\n【時間】\n";
        cout << "  已耗時: " << formatTime(elapsed) << "\n";
        if (queueSize > 0 && stats.totalFiles > 0) {
            cout << "  預估剩餘時間: " << formatTime(remaining) << "\n";
            cout << "  預估總時間: " << formatTime(elapsed + remaining) << "\n";
        }
        
        // 檔案統計
        cout << "\n【檔案統計】\n";
        cout << "  - HTML: " << stats.htmlFiles << "\n";
        cout << "  - 圖片: " << stats.imageFiles << "\n";
        cout << "  - 其他: " << stats.otherFiles << "\n";
        
        // 容量和速度
        cout << "\n【容量與速度】\n";
        cout << "  總容量: " << fixed << setprecision(2) 
             << stats.totalBytes / 1024.0 / 1024.0 << " MB\n";
        
        if (elapsed > 0) {
            double avgSpeed = (stats.totalBytes / 1024.0 / 1024.0) / elapsed;
            cout << "  平均速度: " << fixed << setprecision(2) << avgSpeed << " MB/s\n";
        }
        
        cout << "==============================\n\n";
    }
    
    //  格式化時間顯示
    string formatTime(double seconds) {
        if (seconds < 60) {
            return to_string((int)seconds) + " 秒";
        } else if (seconds < 3600) {
            int minutes = (int)(seconds / 60);
            int secs = (int)seconds % 60;
            return to_string(minutes) + " 分 " + to_string(secs) + " 秒";
        } else {
            int hours = (int)(seconds / 3600);
            int minutes = (int)(seconds / 60) % 60;
            int secs = (int)seconds % 60;
            return to_string(hours) + " 小時 " + to_string(minutes) + " 分 " + to_string(secs) + " 秒";
        }
    }
    
public:
    WebDownloader(const string& url, const string& output, int depth, bool external, 
                  bool documents = false, const set<string>& typeFilter = {}, 
                  long long minSize = 0, long long maxSize = 0)
        : baseURL(url), outputDir(output), maxDepth(depth), downloadExternal(external), 
          downloadDocuments(documents), fileTypeFilter(typeFilter), 
          minFileSize(minSize), maxFileSize(maxSize) {
        URL parsed = parseURL(url);
        baseHost = parsed.host;
        
        cout << "[初始化] 基礎主機: " << baseHost << endl;
        cout << "[設定] 下載外部網站: " << (downloadExternal ? "是" : "否") << endl;
        cout << "[設定] 下載文件檔案: " << (downloadDocuments ? "是" : "否") << endl;
        
        //  顯示檔案類型過濾
        if (!fileTypeFilter.empty()) {
            cout << "[設定] 僅下載檔案類型: ";
            bool first = true;
            for (const auto& type : fileTypeFilter) {
                if (!first) cout << ", ";
                cout << type;
                first = false;
            }
            cout << endl;
        }
        
        //  顯示檔案大小限制
        if (minFileSize > 0) {
            cout << "[設定] 最小檔案大小: " << minFileSize / 1024 << " KB" << endl;
        }
        if (maxFileSize > 0) {
            cout << "[設定] 最大檔案大小: " << maxFileSize / 1024 << " KB" << endl;
        }
        
        //  測試 URL 解析和路徑生成
        testURLProcessing();
    }
    
    // 測試 URL 處理
    void testURLProcessing() {
        cout << "\n[測試] URL 解析和路徑生成" << endl;
        cout << "規則：所有圖片 → images/ 目錄（扁平化）\n" << endl;
        
        vector<string> testURLs = {
            "http://hsccl.us.to/index.html",
            "http://hsccl.us.to/image.jpg",
            "http://hsccl.us.to/images/image.jpg",
            "http://hsccl.us.to/images/animals/cats/image.jpg"
        };
        
        for (const auto& testURL : testURLs) {
            URL parsed = parseURL(testURL);
            string localPath = getLocalPath(testURL);
            
            cout << "  " << testURL << endl;
            cout << "    → " << localPath << endl;
        }
        
        cout << "\n預期結果：" << endl;
        cout << "  hsccl.us.to/index.html" << endl;
        cout << "  hsccl.us.to/images/image.jpg" << endl;
        cout << "  hsccl.us.to/images/images_image.jpg" << endl;
        cout << "  hsccl.us.to/images/cats_image.jpg" << endl;
        cout << endl;
    }
    
    // 下載單個檔案
    bool downloadFile(const string& url, int depth) {
        if (g_interrupted) return false;
        
        // 檢查是否已訪問
        if (visited.find(url) != visited.end()) {
            cout << "[跳過] 已訪問: " << url << endl;
            return true;
        }
        visited.insert(url);
        
        // 檢查是否為外部網站
        URL parsed = parseURL(url);
        bool isExternal = (parsed.host != baseHost);
        
        if (!downloadExternal && isExternal) {
            cout << "[跳過外部] " << url << " (來自 " << parsed.host << ")" << endl;
            return false;
        }
        
        if (isExternal) {
            cout << "[外部網站] " << url << endl;
        }
        
        cout << "\n[下載] " << url << " (深度: " << depth << ")" << endl;
        
        // 建立連接
        int sockfd = connectToHost(parsed.host, parsed.port);
        if (sockfd < 0) {
            cout << "[錯誤] 無法連接到 " << parsed.host << endl;
            return false;
        }
        
        // 發送請求
        string request = buildHTTPRequest(parsed);
        auto startTime = chrono::steady_clock::now();
        string response = sendHTTPRequest(sockfd, request);
        close(sockfd);
        
        if (response.empty()) {
            cout << "[錯誤] 空回應" << endl;
            return false;
        }
        
        // 解析回應
        auto [headers, body] = parseHTTPResponse(response);
        
        // 檢查 HTTP 狀態碼
        if (response.find("200 OK") == string::npos && response.find("200 Ok") == string::npos) {
            cout << "[警告] HTTP 狀態異常" << endl;
        }
        
        // 判斷檔案類型
        string fileType = getFileType(url, headers);
        
        //  HTML 頁面需要特殊處理
        bool isHTML = (fileType == "HTML");
        bool shouldDownload = true;  // 是否應該下載此檔案
        
        //  檢查檔案類型過濾
        if (!isHTML && !isFileTypeAllowed(url)) {
            string ext = getFileExtension(url);
            cout << "[跳過類型] " << url << " (." << ext << ")" << endl;
            shouldDownload = false;
        }
        
        //  檢查檔案大小限制
        long long fileSize = body.length();
        if (!isHTML && shouldDownload && !isFileSizeAllowed(fileSize)) {
            cout << "[跳過大小] " << url << " (" << fileSize / 1024 << " KB)" << endl;
            if (minFileSize > 0 && fileSize < minFileSize) {
                cout << "           (小於 " << minFileSize / 1024 << " KB)" << endl;
            }
            if (maxFileSize > 0 && fileSize > maxFileSize) {
                cout << "           (大於 " << maxFileSize / 1024 << " KB)" << endl;
            }
            shouldDownload = false;
        }
        
        //  檢查是否為文件檔案
        if (fileType == "DOCUMENT" && !downloadDocuments) {
            cout << "[跳過文件] " << url << " (使用 --docs 下載)" << endl;
            shouldDownload = false;
        }
        
        // 計算下載速度
        auto endTime = chrono::steady_clock::now();
        double elapsed = chrono::duration<double>(endTime - startTime).count();
        double speed = elapsed > 0 ? body.length() / elapsed : 0;
        
        displayProgress(url, body.length(), body.length(), speed, fileType);
        cout << " [完成]" << endl;
        
        //  如果是 HTML，轉換連結為本地路徑
        string finalContent = body;
        if (isHTML) {
            finalContent = convertLinksToLocal(body, url);
        }
        
        // 取得本地儲存路徑
        string localPath = getLocalPath(url);
        
        cout << "[URL資訊]" << endl;
        cout << "  原始URL: " << url << endl;
        cout << "  主機: " << parsed.host << endl;
        cout << "  路徑: " << parsed.path << endl;
        cout << "  類型: " << fileType << endl;
        cout << "  本地路徑: " << localPath << endl;
        
        // 只儲存符合條件的檔案
        if (shouldDownload) {
            // 儲存檔案
            if (saveFile(localPath, finalContent)) {
                stats.totalFiles++;
                stats.totalBytes += finalContent.length();
                
                if (fileType == "HTML") stats.htmlFiles++;
                else if (fileType == "IMAGE") stats.imageFiles++;
                else stats.otherFiles++;
                
                downloadStatus[url] = true;
                cout << "[✓ 儲存成功] " << finalContent.length() / 1024 << " KB" << endl;
            } else {
                cout << "[✗ 儲存失敗]" << endl;
            }
        } else {
            cout << "[○ 已處理但未儲存] (用於提取連結)" << endl;
        }
        
        //  關鍵修改：即使深度達到 maxDepth，也要解析 HTML 提取連結
        if (fileType == "HTML") {
            cout << "\n[解析連結] 從 " << url << " (當前深度: " << depth << "/" << maxDepth << ")" << endl;
            vector<string> links = extractLinks(body, url);
            cout << "[找到] " << links.size() << " 個連結\n" << endl;
            
            int htmlLinks = 0, imageLinks = 0, otherLinks = 0;
            int addedToQueue = 0;
            
            for (const string& link : links) {
                if (visited.find(link) == visited.end()) {
                    // 判斷連結類型
                    string linkLower = link;
                    transform(linkLower.begin(), linkLower.end(), linkLower.begin(), ::tolower);
                    
                    bool isHtmlLink = (linkLower.find(".html") != string::npos || 
                                       linkLower.find(".htm") != string::npos);
                    bool isImageLink = (linkLower.find(".png") != string::npos || 
                                        linkLower.find(".jpg") != string::npos || 
                                        linkLower.find(".jpeg") != string::npos ||
                                        linkLower.find(".gif") != string::npos);
                    
                    // 檢查是否為外部連結
                    URL linkUrl = parseURL(link);
                    bool isExternal = (linkUrl.host != baseHost);
                    
                    string typeStr = isHtmlLink ? "HTML" : (isImageLink ? "圖片" : "其他");
                    string externalStr = isExternal ? " [外部]" : "";
                    
                    // HTML 連結只在深度未達上限時加入
                    // 但圖片連結總是加入（深度 +1），這樣可以下載當前頁面的圖片
                    if (isHtmlLink) {
                        htmlLinks++;
                        if (depth < maxDepth) {
                            urlQueue.push({link, depth + 1});
                            addedToQueue++;
                            cout << "  [→ " << typeStr << externalStr << "] " << link << endl;
                        } else {
                            cout << "  [✗ " << typeStr << externalStr << "] " << link << " (深度限制)" << endl;
                        }
                    } else if (isImageLink) {
                        imageLinks++;
                        // 圖片總是下載
                        urlQueue.push({link, depth + 1});
                        addedToQueue++;
                        cout << "  [→ " << typeStr << externalStr << "] " << link << endl;
                    } else {
                        otherLinks++;
                        // 其他資源（CSS, JS 等）
                        if (depth < maxDepth) {
                            urlQueue.push({link, depth + 1});
                            addedToQueue++;
                            cout << "  [→ " << typeStr << externalStr << "] " << link << endl;
                        }
                    }
                } else {
                    cout << "  [跳過] " << link << " (已訪問)" << endl;
                }
            }
            
            cout << "\n統計: HTML=" << htmlLinks << ", 圖片=" << imageLinks 
                 << ", 其他=" << otherLinks << ", 加入佇列=" << addedToQueue << endl;
        }
        
        // 如果不是 HTML 但是是圖片，仍然處理
        else if (fileType == "IMAGE") {
            cout << "[圖片檔案] 無需解析連結" << endl;
        }
        
        return true;
    }
    
    // BFS 下載
    void startDownload() {
        cout << "\n========== 開始下載 ==========\n";
        cout << "目標 URL: " << baseURL << "\n";
        cout << "輸出目錄: " << outputDir << "\n";
        cout << "最大深度: " << maxDepth << "\n";
        cout << "下載外部: " << (downloadExternal ? "是" : "否") << "\n";
        cout << "下載文件: " << (downloadDocuments ? "是" : "否") << "\n";
        cout << "==============================\n";
        
        // 建立輸出目錄
        mkdir(outputDir.c_str(), 0755);
        
        // 初始化佇列
        urlQueue.push({baseURL, 0});
        stats.startTime = chrono::steady_clock::now();
        
        int processedCount = 0;
        auto lastDisplayTime = chrono::steady_clock::now();
        
        // BFS 遍歷
        while (!urlQueue.empty() && !g_interrupted) {
            auto [url, depth] = urlQueue.front();
            urlQueue.pop();
            
            processedCount++;
            
            //  顯示整體進度（每次下載檔案時）
            auto now = chrono::steady_clock::now();
            double elapsed = chrono::duration<double>(now - lastDisplayTime).count();
            
            // 每 2 秒或每 5 個檔案顯示一次進度
            if (elapsed >= 2.0 || processedCount % 5 == 0) {
                displayOverallProgress(processedCount);
                lastDisplayTime = now;
            }
            
            downloadFile(url, depth);
            
            // 定期顯示詳細統計（每 10 個檔案）
            if (stats.totalFiles % 10 == 0 && stats.totalFiles > 0) {
                displayStats();
            }
        }
        
        if (g_interrupted) {
            cout << "\n[中斷] 下載已暫停\n";
            cout << "剩餘 " << urlQueue.size() << " 個 URL 未處理\n";
        } else {
            cout << "\n[完成] 所有檔案下載完成！\n";
        }
        
        displayStats();
    }
    
    //  顯示整體進度（簡化版）
    void displayOverallProgress(int processedCount) {
        int queueSize = urlQueue.size();
        double progress = stats.getProgressPercentage(queueSize);
        double elapsed = stats.getElapsedSeconds();
        double remaining = stats.getEstimatedRemainingSeconds(queueSize);
        
        cout << "\r[整體進度] " << fixed << setprecision(1) << progress << "% | "
             << "已完成: " << stats.totalFiles << " | "
             << "待處理: " << queueSize << " | "
             << "已耗時: " << formatTime(elapsed) << " | "
             << "預估剩餘: " << formatTime(remaining)
             << "        " << flush;
    }
    
    // 繼續下載
    void resumeDownload() {
        g_interrupted = false;
        cout << "\n[繼續] 恢復下載...\n";
        startDownload();
    }
    
    // 重新下載
    void restartDownload() {
        visited.clear();
        downloadStatus.clear();
        while (!urlQueue.empty()) urlQueue.pop();
        stats = Stats();
        
        // 刪除輸出目錄
        string cmd = "rm -rf " + outputDir;
        system(cmd.c_str());
        
        cout << "\n[重新開始] 清除所有下載記錄...\n";
        startDownload();
    }
};

// 命令列模式
void commandLineMode(int argc, char* argv[]) {
    if (argc < 3) {
        cout << "用法: " << argv[0] << " <URL> <輸出目錄> [選項]\n";
        cout << "\n基本選項:\n";
        cout << "  -d <深度>         遞迴深度 (預設: 0)\n";
        cout << "  -e                下載外部網站\n";
        cout << "  --docs            下載文件檔案 (PDF, PPTX 等)\n";
        cout << "\n進階功能:\n";
        cout << "  --type <類型>     僅下載指定類型 (可多次使用)\n";
        cout << "                    例: --type jpg --type png\n";
        cout << "  --min-size <KB>   最小檔案大小 (KB)\n";
        cout << "  --max-size <KB>   最大檔案大小 (KB)\n";
        cout << "\n範例:\n";
        cout << "  " << argv[0] << " http://example.com output -d 3 -e\n";
        cout << "  " << argv[0] << " http://example.com output --type jpg --type png\n";
        cout << "  " << argv[0] << " http://example.com output --min-size 100 --max-size 5000\n";
        return;
    }
    
    string url = argv[1];
    string output = argv[2];
    int depth = 0;
    bool external = false;
    bool documents = false;
    set<string> typeFilter;
    long long minSize = 0;
    long long maxSize = 0;
    
    // 解析選項
    for (int i = 3; i < argc; i++) {
        string arg = argv[i];
        if (arg == "-d" && i + 1 < argc) {
            depth = stoi(argv[++i]);
        } else if (arg == "-e") {
            external = true;
        } else if (arg == "--docs") {
            documents = true;
        } else if (arg == "--type" && i + 1 < argc) {
            string type = argv[++i];
            // 移除開頭的點（如果有）
            if (!type.empty() && type[0] == '.') {
                type = type.substr(1);
            }
            // 轉換為小寫
            transform(type.begin(), type.end(), type.begin(), ::tolower);
            typeFilter.insert(type);
        } else if (arg == "--min-size" && i + 1 < argc) {
            minSize = stoll(argv[++i]) * 1024;  // 轉換為 bytes
        } else if (arg == "--max-size" && i + 1 < argc) {
            maxSize = stoll(argv[++i]) * 1024;  // 轉換為 bytes
        }
    }
    
    WebDownloader downloader(url, output, depth, external, documents, typeFilter, minSize, maxSize);
    
    // 設定信號處理
    signal(SIGINT, signalHandler);
    
    downloader.startDownload();
    
    // 詢問是否繼續或重新下載
    if (g_interrupted) {
        cout << "\n選項:\n";
        cout << "1. 繼續下載\n";
        cout << "2. 重新下載\n";
        cout << "3. 退出\n";
        cout << "請選擇: ";
        
        int choice;
        cin >> choice;
        
        if (choice == 1) {
            downloader.resumeDownload();
        } else if (choice == 2) {
            downloader.restartDownload();
        }
    }
}

// 互動模式
void interactiveMode() {
    string url, output;
    int depth;
    char externalChoice, documentsChoice, filterChoice, sizeChoice;
    
    cout << "\n========== 互動模式 ==========\n";
    cout << "請輸入 URL: ";
    cin >> url;
    
    cout << "請輸入輸出目錄: ";
    cin >> output;
    
    cout << "請輸入遞迴深度 (0 表示不遞迴): ";
    cin >> depth;
    
    cout << "是否下載外部網站？(y/n): ";
    cin >> externalChoice;
    bool external = (externalChoice == 'y' || externalChoice == 'Y');
    
    cout << "是否下載文件檔案 (PDF, PPTX 等)？(y/n): ";
    cin >> documentsChoice;
    bool documents = (documentsChoice == 'y' || documentsChoice == 'Y');
    
    // 檔案類型過濾
    set<string> typeFilter;
    cout << "\n是否設定檔案類型過濾？(y/n): ";
    cin >> filterChoice;
    if (filterChoice == 'y' || filterChoice == 'Y') {
        cout << "請輸入允許的檔案類型（用空格分隔，例如: jpg png gif）: ";
        cin.ignore();  // 清除緩衝區
        string typesInput;
        getline(cin, typesInput);
        
        stringstream ss(typesInput);
        string type;
        while (ss >> type) {
            // 移除開頭的點
            if (!type.empty() && type[0] == '.') {
                type = type.substr(1);
            }
            // 轉換為小寫
            transform(type.begin(), type.end(), type.begin(), ::tolower);
            typeFilter.insert(type);
        }
        
        cout << "已設定過濾類型: ";
        for (const auto& t : typeFilter) {
            cout << "." << t << " ";
        }
        cout << endl;
    }
    
    // 檔案大小限制
    long long minSize = 0, maxSize = 0;
    cout << "\n是否設定檔案大小限制？(y/n): ";
    cin >> sizeChoice;
    if (sizeChoice == 'y' || sizeChoice == 'Y') {
        int minKB, maxKB;
        cout << "請輸入最小檔案大小 (KB，0 表示無限制): ";
        cin >> minKB;
        minSize = minKB * 1024;
        
        cout << "請輸入最大檔案大小 (KB，0 表示無限制): ";
        cin >> maxKB;
        maxSize = maxKB * 1024;
    }
    
    WebDownloader downloader(url, output, depth, external, documents, typeFilter, minSize, maxSize);
    
    // 設定信號處理
    signal(SIGINT, signalHandler);
    
    downloader.startDownload();
    
    // 詢問是否繼續或重新下載
    if (g_interrupted) {
        cout << "\n選項:\n";
        cout << "1. 繼續下載\n";
        cout << "2. 重新下載\n";
        cout << "3. 退出\n";
        cout << "請選擇: ";
        
        int choice;
        cin >> choice;
        
        if (choice == 1) {
            downloader.resumeDownload();
        } else if (choice == 2) {
            downloader.restartDownload();
        }
    }
}

int main(int argc, char* argv[]) {
    cout << "========================================\n";
    cout << "     網頁離線下載器 v1.1\n";
    cout << "========================================\n";
    
    if (argc > 1) {
        // 命令列模式
        commandLineMode(argc, argv);
    } else {
        // 互動模式
        interactiveMode();
    }
    
    return 0;
}

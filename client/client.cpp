#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <vector>
#include <pthread.h>
#include <algorithm>
#include <stdio.h>
#include <signal.h>
#include <atomic>
#include <sstream>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdexcept>
#include <openssl/sha.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <unordered_map>
#include <sys/types.h>
#include <iomanip>


using namespace std;

std::atomic<bool> isExit(false);

const long BLOCKSIZE = 512 * 1024;

struct FileInfo {
    string fileName;
    size_t fileSize;
    string fileHash;
    vector<string> chunkHashes;
    string relativePath;
};

struct DownloadedFileInfo {
    string fileName;
    string location;
    string groupName;
    string status;
};

vector<DownloadedFileInfo> downloads;
unordered_map<string, FileInfo> sharedFiles;
mutex sharedFilesMutex;
mutex downloadsMutex;

string bytesToHexString(const unsigned char* bytes, int length) {
    string hexString;
    for (int i = 0; i < length; i++) {
        char hex[3];
        snprintf(hex, sizeof(hex), "%02x", bytes[i]);
        hexString += hex;
    }
    return hexString;
}

string calculateTotalFileHash(const string& fileName) {
    int fd = open(fileName.c_str(), O_RDONLY);
    if (fd == -1) {
        throw runtime_error("Unable to open file: " + fileName);
    }
    SHA_CTX sha1Context;
    SHA1_Init(&sha1Context);

    const size_t bufferSize = 8192;
    unsigned char buffer[bufferSize];
    ssize_t bytesRead;

    while ((bytesRead = read(fd, buffer, bufferSize)) > 0) {
        SHA1_Update(&sha1Context, buffer, bytesRead);
    }

    if (bytesRead == -1) {
        close(fd);
        throw runtime_error("Error reading file: " + fileName);
    }

    unsigned char hash[SHA_DIGEST_LENGTH];
    SHA1_Final(hash, &sha1Context);
    close(fd);

    return bytesToHexString(hash, SHA_DIGEST_LENGTH);
}

vector<string> calculateChunkHashes(const string& fileName) {
    int fd = open(fileName.c_str(), O_RDONLY);
    if (fd == -1) {
        cout << "Unable to find the file!" << "\n";
    }

    vector<string> chunkHashes;
    unsigned char buffer[BLOCKSIZE];
    ssize_t bytesRead;

    while ((bytesRead = read(fd, buffer, BLOCKSIZE)) > 0) {
        SHA_CTX sha1Context;
        SHA1_Init(&sha1Context);
        SHA1_Update(&sha1Context, buffer, bytesRead);

        unsigned char hash[SHA_DIGEST_LENGTH];
        SHA1_Final(hash, &sha1Context);

        chunkHashes.push_back(bytesToHexString(hash, SHA_DIGEST_LENGTH));
    }
    if (bytesRead == -1) {
        close(fd);
        cout << "Error Reading the file data!" << endl;
    }

    close(fd);
    return chunkHashes;
}

void signalHandler(int signum) {
    isExit.store(true);
}

struct ClientDetails {
    string ip;
    int port;
};

void informTrackerNewSeeder(int trackerSocket, const string& groupName, const string& fileName, const string& fileHash, const string& filePath) {
    string msg = "new_seeder " + groupName + " " + fileName + " " + fileHash + " " + filePath;
    send(trackerSocket, msg.c_str(), msg.size(), 0);
    char response[1024*1024];
    recv(trackerSocket, response, sizeof(response), 0);
    cout << "Tracker response: " << response << endl;
}

vector<pair<string, int>> parseSeederInfo(const string& seederInfo) {
    vector<pair<string, int>> seeders;
    istringstream iss(seederInfo);
    string line;
    while (getline(iss, line)) {
        istringstream lineStream(line);
        string username, ip;
        int port;
        if (lineStream >> username >> ip >> port) {
            seeders.emplace_back(ip, port);
        }
    }
    return seeders;
}

void handleChunkRequest(int clientSocket) {
    char buffer[1024*1024];
    ssize_t bytesRead = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
    if (bytesRead <= 0) {
        close(clientSocket);
        return;
    }
    buffer[bytesRead] = '\0';

    string request(buffer);
    istringstream iss(request);
    string command, fileName;
    int chunkIndex;
    string relativePath;
    iss >> command >> fileName >> chunkIndex >> relativePath;

    if (command != "send_chunk") {
        close(clientSocket);
        return;
    }

    lock_guard<mutex> lock(sharedFilesMutex);
    auto it = sharedFiles.find(fileName);
    if (it == sharedFiles.end()) {
        close(clientSocket);
        return;
    }

    string fullPath = relativePath + "/" + fileName;
    int fd = open(fullPath.c_str(), O_RDONLY);
    if (fd == -1) {
        close(clientSocket);
        return;
    }

    off_t offset = static_cast<off_t>(chunkIndex) * BLOCKSIZE;
    if (lseek(fd, offset, SEEK_SET) == -1) {
        close(fd);
        close(clientSocket);
        return;
    }

    char chunkBuffer[BLOCKSIZE];
    ssize_t bytesReadFromFile = read(fd, chunkBuffer, BLOCKSIZE);
    if (bytesReadFromFile > 0) {
        ssize_t bytesSent = 0;
        while (bytesSent < bytesReadFromFile) {
            ssize_t result = send(clientSocket, chunkBuffer + bytesSent, bytesReadFromFile - bytesSent, 0);
            if (result <= 0) {
                break;
            }
            bytesSent += result;
        }
    }

    close(fd);
    close(clientSocket);
}

void* listenClient(void* arg) {
    ClientDetails* details = static_cast<ClientDetails*>(arg);
    int listenSockClient = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSockClient < 0) {
        cerr << "Socket Creation Failed!" << endl;
        isExit = true;
        return nullptr;
    }
    sockaddr_in hint;
    hint.sin_addr.s_addr = inet_addr(details->ip.c_str());
    hint.sin_family = AF_INET;
    hint.sin_port = htons(details->port);

    if (bind(listenSockClient, (sockaddr*)&hint, sizeof(hint)) < 0) {
        cerr << "Binding Socket with PORT " << details->port << " Failed!" << endl;
        close(listenSockClient);
        isExit = true;
        return nullptr;
    }
    if (listen(listenSockClient, 128) < 0) {
        cerr << "Listening on port " << details->port << " Failed!" << endl;
        close(listenSockClient);
        isExit = true;
        return nullptr;
    }

    while (!isExit.load()) {
        sockaddr_in client;
        socklen_t clientSize = sizeof(client);
        int clientSocket = accept(listenSockClient, (sockaddr*)&client, &clientSize);
        if (clientSocket == -1) {
            continue;
        }

        thread(handleChunkRequest, clientSocket).detach();
    }
    close(listenSockClient);
    return nullptr;
}

bool downloadChunksFromSeeder(const string& seederIP, int seederPort, const string& fileName, int chunkIndex, vector<char>& chunkData, const string& relativePath) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        cerr << "Socket creation failed" << endl;
        return false;
    }

    sockaddr_in seederAddr;
    seederAddr.sin_family = AF_INET;
    seederAddr.sin_port = htons(seederPort);
    if(inet_pton(AF_INET, seederIP.c_str(), &seederAddr.sin_addr) <= 0) {
        close(sock);
        return false;
    }

    if (connect(sock, (struct sockaddr *)&seederAddr, sizeof(seederAddr)) < 0) {
        cerr << "Connection to seeder failed" << endl;
        close(sock);
        return false;
    }

    string request = "send_chunk " + fileName + " " + to_string(chunkIndex) + " " + relativePath;
    send(sock, request.c_str(), request.length(), 0);

    chunkData.resize(BLOCKSIZE);
    int bytesReceived = 0;
    while (bytesReceived < BLOCKSIZE) {
        int result = recv(sock, chunkData.data() + bytesReceived, BLOCKSIZE - bytesReceived, 0);
        if (result <= 0) {
            if (bytesReceived > 0) break;  
            cerr << "Failed to receive chunk" << endl;
            close(sock);
            return false;
        }
        bytesReceived += result;
    }
    chunkData.resize(bytesReceived);
    close(sock);
    return true;
}

bool downloadFileFromSeeders(const string& fileName, size_t fileSize, const string& fileHash, const vector<string>& chunkHashes, const vector<pair<string, int>>& seeders, const vector<pair<string, pair<int, int>>>& partialSeeders, string& destPath, const string& groupName, int trackerSocket) {

    string fileLocationAtSeeder = "sharedFilesFolder/" + groupName;

    vector<pair<string, int>> allAvailableSeeders = seeders;

    int numChunks = (fileSize + BLOCKSIZE - 1) / BLOCKSIZE;
    vector<vector<char>> chunks(numChunks);
    vector<bool> chunkDownloaded(numChunks, false);

    
    for (const auto& partialSeeder : partialSeeders) {
        allAvailableSeeders.emplace_back(partialSeeder.first, partialSeeder.second.second);
    }

    size_t seederIndex = 0;

    for (int i = 0; i < numChunks; i++) {
        bool chunkDownloaded = false;
        int initi = 0;

        while (!chunkDownloaded && initi < allAvailableSeeders.size()) {
            const auto& seeder = allAvailableSeeders[seederIndex];
            bool isPartialSeeder = seederIndex >= seeders.size();
            if (isPartialSeeder) {
                auto it = find_if(partialSeeders.begin(), partialSeeders.end(),
                    [&](const auto& ps) { return ps.first == seeder.first && ps.second.second == seeder.second; });
                if (it == partialSeeders.end() || it->second.first != i) {
                    seederIndex = (seederIndex + 1) % allAvailableSeeders.size();
                    initi++;
                    continue;
                }
            }

            if (downloadChunksFromSeeder(seeder.first, seeder.second, fileName, i, chunks[i], fileLocationAtSeeder)) {
                string calculatedHash = bytesToHexString(SHA1((unsigned char*)chunks[i].data(), chunks[i].size(), nullptr), SHA_DIGEST_LENGTH);
                if (calculatedHash == chunkHashes[i]) {
                    chunkDownloaded = true;
                    cout << "Chunk " << i << " downloaded from " << (isPartialSeeder ? "partial" : "full") << " seeder " << seeder.first << ":" << seeder.second << endl;
                }
            }

            seederIndex = (seederIndex + 1) % allAvailableSeeders.size();
            initi++;
        }

        if (!chunkDownloaded) {
            cerr << "Failed to download chunk " << i << endl;
            return false;
        }
        
        string updateChunkMsg = "add_leecher " + fileHash + " " + to_string(i);
        send(trackerSocket, updateChunkMsg.c_str(), updateChunkMsg.size(), 0);
        char response[1024*1024];
        recv(trackerSocket, response, sizeof(response), 0);
        cout << "Tracker response for chunk update: " << response << endl;
    }

    string fullPath = destPath;
    int fd = open(fullPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        cerr << "Failed to create output file" << endl;
        return false;
    }

    for (const auto& chunk : chunks) {
        if (write(fd, chunk.data(), chunk.size()) == -1) {
            cerr << "Failed to write chunk to file" << endl;
            close(fd);
            return false;
        }
    }
    close(fd);

    mkdir("sharedFilesFolder", 0777);
    mkdir(fileLocationAtSeeder.c_str(), 0777);
    fullPath = fileLocationAtSeeder + "/" + fileName;
    fd = open(fullPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) {
        cerr << "Failed to create shared file" << endl;
        return false;
    }

    for (const auto& chunk : chunks) {
        if (write(fd, chunk.data(), chunk.size()) == -1) {
            cerr << "Failed to write chunk to shared file" << endl;
            close(fd);
            return false;
        }
    }
    close(fd);

    string calculatedFileHash = calculateTotalFileHash(fullPath);
    if (calculatedFileHash != fileHash) {
        cerr << "File hash mismatch." << endl;
        return false;
    }
    informTrackerNewSeeder(trackerSocket, groupName, fileName, fileHash, fullPath);

    cout << "File download completed and verification passed. Informed tracker about new seeder." << "\n" << "Added file to sharedFolder" << endl;
    return true;
}


vector<pair<string,int>> parseTrackerFile(string filePath) {
    char buff[50];
    vector<pair<string, int>> res;

    int fd = open(filePath.c_str(), O_RDONLY);

    while (true) {
        int bytesRead = read(fd, buff, sizeof(buff) - 1);
        if (bytesRead <= 0) {
            break;
        }

        buff[bytesRead] = '\0';
        stringstream ss(buff);
        string line;
        while (getline(ss, line)) {
            if (line.empty()) {
                continue;
            }
            int s = line.find_first_of(' ');
            if (s != string::npos) {
                string ip = line.substr(0, s);
                string portStr = line.substr(s + 1);
                int port = stoi(portStr);
                res.push_back({ip, port});
            }
        }
    }
    close(fd);
    return res;
}

pair<string,int> checkTrackerStatus(string filePath) {
    vector<pair<string,int>> result = parseTrackerFile(filePath);
    for(auto p: result) {
        string trackerIP = p.first;
        int trackerPort = p.second;
        int sendSockClient = socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in hint;
        cout << trackerIP << " " << trackerPort << endl;
        hint.sin_family = AF_INET;
        hint.sin_port = htons(trackerPort);
        hint.sin_addr.s_addr = inet_addr(trackerIP.c_str());
        if(connect(sendSockClient, (sockaddr*)&hint, sizeof(hint)) != -1) {
            close(sendSockClient);
            return p;
        }
    }
    cout << "No Tracker is up and running!" << endl;
    exit(0);   
}

void stopSharingFile(const string& fileName) {
    lock_guard<mutex> lock(sharedFilesMutex);
    auto it = sharedFiles.find(fileName);
    if (it != sharedFiles.end()) {
        string filePath = it->second.relativePath + "/" + fileName;
        if (remove(filePath.c_str()) == 0) {
            sharedFiles.erase(it);
            cout << "File removed from shared files: " << fileName << endl;
        } else {
            // cerr << "Error removing file: " << fileName << endl;
        }
    } else {
        cout << "File not found in shared files: " << fileName << endl;
    }
}

void showDownloads() {
    lock_guard<mutex> lock(downloadsMutex);
    if (downloads.empty()) {
        cout << "Not downloading anything yet!" << endl;
        return;
    }    
    for (const auto& download : downloads) {
        cout <<"[" << download.status << "]"<<" "<<download.groupName <<" "<< download.fileName<<endl;
    }
    cout << endl;
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        cerr << "Need 2 arguments =>> <IP:Listening PORT> <TrackerInfo File Path>!" << endl;
        return -1;
    }
    
    string ipPortc = argv[1];
    int x = ipPortc.find_first_of(':');
    string ip = ipPortc.substr(0,x);
    string tempPort = ipPortc.substr(x+1,ipPortc.length()-x+1);
    string myIP = ip;
    int myPort = 9999;

    pair<string,int> data = checkTrackerStatus(argv[2]);
    string trackerIP = data.first;
    int trackerPort = data.second;
    
    try {
        myPort = stoi(tempPort);
    } 
    catch(const exception& e) {
        cerr << e.what() << endl;
        return -1;
    }
    
    signal(SIGINT, signalHandler);
    
    pthread_t listenerThread;
    ClientDetails* d1 = new ClientDetails{myIP, myPort};
    
    int result = pthread_create(&listenerThread, nullptr, listenClient, static_cast<void*>(d1));
    if (result != 0) {
        delete d1;
        cout << "Thread creation failed" << endl;
        return -1;
    }

    int sendSockClient = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in hint;
    hint.sin_family = AF_INET;
    hint.sin_port = htons(trackerPort);
    hint.sin_addr.s_addr = inet_addr(trackerIP.c_str());

    if(connect(sendSockClient, (sockaddr*)&hint, sizeof(hint)) == -1) {
        cout << "Establishing Communication Pipe with Tracker Failed!" << endl;
        delete d1;
        return -1;
    }
    string ipPort = myIP + ":" + to_string(myPort);

    send(sendSockClient, ipPort.c_str(), ipPort.size(), 0);

    string msg;
    char rbuff[1024*1024];
    while (!isExit.load()) {
        cout << "$>";
        getline(cin, msg);
        if (msg == "exit" || msg == "quit" || isExit.load()) {
            break;
        }
        istringstream iss(msg);
        string cmd, filePath, groupName;
        iss >> cmd >> groupName >> filePath;
        if(cmd == "upload_file") {
            struct stat st;
            if (stat(filePath.c_str(), &st) != 0) {
                cerr << "File not found: " << filePath << endl;
                continue;
            }
            size_t size = st.st_size;
            vector<string> chunkHashes = calculateChunkHashes(filePath);
            string totalFileHash = calculateTotalFileHash(filePath);
            string concatHashes = chunkHashes[0];
            for(int i = 1; i < chunkHashes.size(); i++) {
                concatHashes += "+";
                concatHashes += chunkHashes[i];
            }
            string fileName;
            int pos = filePath.find_last_of("/\\");
            if(pos == string::npos) fileName = filePath;
            else fileName = filePath.substr(pos+1);
            string sharedDir = "sharedFilesFolder/" + groupName;
            mkdir("sharedFilesFolder", 0777);
            mkdir(sharedDir.c_str(), 0777);

            string destPath = sharedDir + "/" + fileName;
            int srcFd = open(filePath.c_str(), O_RDONLY);
            int destFd = open(destPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (srcFd == -1 || destFd == -1) {
                cerr << "Error opening files for copy" << endl;
                if (srcFd != -1) close(srcFd);
                if (destFd != -1) close(destFd);
                continue;
            }

            char buffer[8192];
            ssize_t bytesRead, bytesWritten;
            while ((bytesRead = read(srcFd, buffer, sizeof(buffer))) > 0) {
                bytesWritten = write(destFd, buffer, bytesRead);
                if (bytesWritten != bytesRead) {
                    cerr << "Error writing to destination file" << endl;
                    close(srcFd);
                    close(destFd);
                    continue;
                }
            }

            close(srcFd);
            close(destFd);

            msg = "upload_file " + groupName + " " + fileName + " " + to_string(size) + " " + totalFileHash + " " + concatHashes + " " + sharedDir;
            lock_guard<mutex> lock(sharedFilesMutex);
            sharedFiles[fileName] = {fileName, size, totalFileHash, chunkHashes, sharedDir};
        }
        else if(cmd == "download_file") {
            istringstream issd(msg);
            string cmd, gn, fn, destPath;
            issd >> cmd >> gn >> fn >> destPath;
            send(sendSockClient, msg.c_str(), msg.size(), 0);
            int br = recv(sendSockClient, rbuff, sizeof(rbuff), 0);
            rbuff[br] = '\0';
            string response(rbuff);
            cout << "Received From Tracker: " << response << endl;

            istringstream iss(response);
            string line;
            string fileHash;
            size_t fileSize;
            vector<string> chunkHashes;
            vector<pair<string, int>> seeders;
            vector<pair<string, pair<int, int>>> partialSeeders;

            while (getline(iss, line)) {
                if (line.find("FileHash: ") == 0) {
                    fileHash = line.substr(10);
                } else if (line.find("FileSize: ") == 0) {
                    fileSize = stoul(line.substr(10));
                } else if (line.find("ChunkHashes: ") == 0) {
                    istringstream chunkStream(line.substr(12));
                    string chunkHash;
                    while (chunkStream >> chunkHash) {
                        chunkHashes.push_back(chunkHash);
                    }
                } else if (line.find("Seeders:") == 0) {
                    while (getline(iss, line) && !line.empty()) {
                        istringstream seederStream(line);
                        string username, ip;
                        int port;
                        seederStream >> username >> ip >> port;
                        seeders.emplace_back(ip, port);
                    }
                } else if (line.find("PartialSeeders:") == 0) {
                    while (getline(iss, line) && !line.empty()) {
                        istringstream seederStream(line);
                        string username, ip;
                        int port, chunkIndex;
                        seederStream >> username >> ip >> port >> chunkIndex;
                        partialSeeders.emplace_back(ip, make_pair(chunkIndex, port));
                    }
                }
            }

            DownloadedFileInfo dfi;
            dfi.fileName = fn;
            dfi.groupName = gn;

            if (downloadFileFromSeeders(fn, fileSize, fileHash, chunkHashes, seeders, partialSeeders, destPath, gn, sendSockClient)) {
                cout << "File downloaded successfully: " << fn << endl;
                dfi.location = destPath;
                dfi.status = "C";
                lock_guard<mutex> lock(sharedFilesMutex);
                sharedFiles[fn] = {fn, fileSize, fileHash, chunkHashes, destPath};
            } 
            else {
                cout << "File download failed, check seeders status!" << endl;
                dfi.location = "Download Failed";
                dfi.status = "I";
            }

            lock_guard<mutex> lock(downloadsMutex);
            downloads.push_back(dfi);
            continue;
        }
        else if(cmd == "show_downloads") {
            showDownloads();
            continue;
        }

        else if(cmd == "stop_share") {
            istringstream issd(msg);
            string cmd, gn, fn;
            issd >> cmd >> gn >> fn;
            send(sendSockClient, msg.c_str(), msg.size(), 0);
            int br = recv(sendSockClient, rbuff, sizeof(rbuff), 0);
            rbuff[br] = '\0';
            string response(rbuff);
            cout << "Received From Tracker: " << response << endl;
            if (response == "Stopped sharing the file successfully") {
                stopSharingFile(fn);
            }
            continue;
        }
        send(sendSockClient, msg.c_str(), msg.size(), 0);
        int br = recv(sendSockClient, rbuff, sizeof(rbuff), 0);
        rbuff[br] = '\0';
        cout << "Received From Tracker: " << string(rbuff) << endl;
    }
    close(sendSockClient);
    pthread_cancel(listenerThread);
    pthread_join(listenerThread, nullptr);
    delete d1;
    return 0;
}
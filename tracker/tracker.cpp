#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <vector>
#include <pthread.h>
#include <algorithm>
#include <signal.h>
#include <atomic>
#include <mutex>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <ctime>
#include <fcntl.h>
#include <condition_variable>
#include <iomanip>

using namespace std;

int listenSockServer = -1;
int logFD = -1;
atomic<bool> shouldExit(false);
condition_variable cv;
mutex cv_m;
unordered_set<int> openFds;
vector<pthread_t> client_threads;

void signalInterruptHandler(int signum) {
    shouldExit.store(true);
    cv.notify_all();
}

struct User {
    string username;
    string password;
    bool isLoggedIn;
    unordered_set<string> groups;
    string ip;
    string port;
};

struct Group {
    string groupId;
    string owner;
    unordered_set<string> members;
    unordered_set<string> pendingRequests;
    unordered_set<string> fileHashes; 
};

struct ClientInfo {
    int socket;
    string ip;
    string port;
    string username;
};

struct Chunk {
    string chunkHash;
    unordered_set<string> seeders; 
};

struct File {
    string fileName;
    string fileHash;
    size_t fileSize;
    vector<Chunk> chunks;
    unordered_map<string, string> seeders;
    unordered_map<string, vector<int>> partialSeeders;
};

unordered_map<string, shared_ptr<User>> users;
unordered_map<string, shared_ptr<Group>> groups;
unordered_map<string, shared_ptr<File>> files;
mutex filesMutex;
mutex usersMutex;
mutex groupsMutex;

void log(const string& message) {
    static mutex log_mutex;
    lock_guard<mutex> lock(log_mutex);
    time_t now = time(0);
    tm *ltm = localtime(&now);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", ltm);
    string log_message = string(timestamp) + " - " + message + "\n";
    write(logFD, log_message.c_str(), log_message.length());
}

string createUser(const string& username, const string& password) {
    unique_lock<mutex> lock(usersMutex);
    
    if (username.empty() || password.empty()) {
        return "Username and password cannot be empty";
    }
    
    for (const auto& [existingUsername, _] : users) {
        if (existingUsername == username) {
            log("User creation failed: " + username + " (already exists)");
            return "User already exists";
        }
    }
    
    users.emplace(username, make_shared<User>(User{username, password, false, {}, "", ""}));
    
    log("User created: " + username);
    return "User created successfully";
}

string login(const string& username, const string& password, ClientInfo* client) {
    unique_lock<mutex> lock(usersMutex);
    auto it = users.find(username);
    if (it == users.end() || it->second->password != password) {
        log("Login failed: " + username + " (invalid credentials)");
        return "Invalid credentials";
    }
    if (it->second->isLoggedIn) {
        log("Login failed: " + username + " (already logged in)");
        return "User already logged in";
    }
    it->second->isLoggedIn = true;
    client->username = username;
    it->second->ip = client->ip;
    it->second->port = client->port;
    log("User logged in: " + username);
    return "Login successful";
}

string logout(ClientInfo* client) {
    unique_lock<mutex> lock(usersMutex);
    if (client->username.empty()) {
        return "Not logged in";
    }
    auto it = users.find(client->username);
    if (it != users.end()) {
        it->second->isLoggedIn = false;
    }
    string username = client->username;
    client->username.clear();
    log("User logged out: " + username);
    return "Logout successful";
}

string createGroup(const string& username, const string& groupId) {
    unique_lock<mutex> lockUsers(usersMutex);
    unique_lock<mutex> lockGroups(groupsMutex);
    
    auto userIt = users.find(username);
    if (userIt == users.end()) {
        log("Group creation failed: " + groupId + " (user " + username + " not found)");
        return "User not found";
    }
    
    if (groups.find(groupId) != groups.end()) {
        log("Group creation failed: " + groupId + " (already exists)");
        return "Group already exists";
    }
    
    groups[groupId] = make_shared<Group>(Group{groupId, username, {username}, {}});
    userIt->second->groups.insert(groupId);
    log("Group created: " + groupId + " by " + username);
    return "Group created successfully";
}

string joinGroup(const string& username, const string& groupId) {
    unique_lock<mutex> lock(groupsMutex);
    auto it = groups.find(groupId);
    if (it == groups.end()) {
        log("Join group failed: " + groupId + " (group does not exist)");
        return "Group does not exist";
    }
    if (it->second->members.find(username) != it->second->members.end()) {
        log("Join group failed: " + username + " (already a member of " + groupId + ")");
        return "Already a member of the group";
    }
    it->second->pendingRequests.insert(username);
    log("Join request sent: " + username + " for group " + groupId);
    return "Join request sent";
}

string leaveGroup(const string& username, const string& groupId) {
    unique_lock<mutex> lockGroups(groupsMutex);
    unique_lock<mutex> lockFiles(filesMutex);
    auto it = groups.find(groupId);
    if (it == groups.end()) {
        log("Leave group failed: " + groupId + " (group does not exist)");
        return "Group does not exist";
    }
    if (it->second->members.find(username) == it->second->members.end()) {
        log("Leave group failed: " + username + " (not a member of " + groupId + ")");
        return "Not a member of the group";
    }
    for (const auto& fileHash : it->second->fileHashes) {
        auto fileIt = files.find(fileHash);
        if (fileIt != files.end()) {
            fileIt->second->seeders.erase(username);
            fileIt->second->partialSeeders.erase(username);
            for (auto& chunk : fileIt->second->chunks) {
                chunk.seeders.erase(username);
            }
            if (fileIt->second->seeders.empty() && fileIt->second->partialSeeders.empty()) {
                it->second->fileHashes.erase(fileHash);
                files.erase(fileIt);
                log("File removed: " + fileIt->second->fileName + " (no seeders left in group " + groupId + ")");
            }
        }
    }

    if (it->second->owner == username) {
        if (it->second->members.size() <= 1) {
            groups.erase(groupId);
            log("Group deleted: " + groupId + " (last member left)");
        } else {
            auto newOwner = find_if(it->second->members.begin(), it->second->members.end(),
                                    [&username](const string& member) { return member != username; });
            if (newOwner != it->second->members.end()) {
                it->second->owner = *newOwner;
                log("New owner assigned for group " + groupId + ": " + *newOwner);
            }
        }
    }
    it->second->members.erase(username);
    users[username]->groups.erase(groupId);
    log("User left group: " + username + " from " + groupId);
    return "Left group successfully";
}

string listRequests(const string& username, const string& groupId) {
    unique_lock<mutex> lock(groupsMutex);
    auto it = groups.find(groupId);
    if (it == groups.end()) {
        log("List requests failed: " + groupId + " (group does not exist)");
        return "Group does not exist";
    }
    if (it->second->owner != username) {
        log("List requests failed: " + username + " (not owner of " + groupId + ")");
        return "You don't have permission to access this info!";
    }
    string result = "Pending requests: ";
    for (const auto& req : it->second->pendingRequests) {
        result += req + " ";
    }
    log("Listed requests for group " + groupId);
    return result;
}

string acceptRequest(const string& groupId, const string& username) {
    unique_lock<mutex> lock(groupsMutex);
    auto it = groups.find(groupId);
    if (it == groups.end()) {
        log("Accept request failed: " + groupId + " (group does not exist)");
        return "Group does not exist";
    }
    if (it->second->pendingRequests.erase(username) == 0) {
        log("Accept request failed: " + username + " (no pending request for " + groupId + ")");
        return "No pending request from this user";
    }
    it->second->members.insert(username);
    users[username]->groups.insert(groupId);
    log("User added to group: " + username + " to " + groupId);
    return "User added to group";
}

string addNewSeeder(const string& username, const string& groupName, const string& fileName, const string& fileHash) {
    unique_lock<mutex> lockGroups(groupsMutex);
    unique_lock<mutex> lockFiles(filesMutex);
    
    auto groupIt = groups.find(groupName);
    if (groupIt == groups.end()) {
        return "Group not found";
    }
    
    if (groupIt->second->members.find(username) == groupIt->second->members.end()) {
        return "You are not a member of this group";
    }
    
    auto fileIt = files.find(fileHash);
    if (fileIt == files.end()) {
        return "File not found";
    }
    
    string relativePath = "shared_files/" + groupName;
    fileIt->second->seeders[username] = relativePath;
    
    log("New seeder added: " + username + " for file " + fileName + " in group " + groupName);
    return "Seeder information updated successfully";
}

string downloadFile(const string& username, const string& groupName, const string& fileName) {
    unique_lock<mutex> lockGroups(groupsMutex);
    unique_lock<mutex> lockFiles(filesMutex);
    unique_lock<mutex> lockUsers(usersMutex);
    
    auto groupIt = groups.find(groupName);
    if (groupIt == groups.end()) {
        return "Group not found";
    }
    if (groupIt->second->members.find(username) == groupIt->second->members.end()) {
        return "You are not a member of this group";
    }
    
    string fileHash;
    for (const auto& hash : groupIt->second->fileHashes) {
        auto fileIt = files.find(hash);
        if (fileIt != files.end() && fileIt->second->fileName == fileName) {
            fileHash = hash;
            break;
        }
    }
    
    if (fileHash.empty()) {
        return "File not found in the group";
    }
    
    auto fileIt = files.find(fileHash);
    if (fileIt == files.end()) {
        return "File information not found";
    }
 
    stringstream ss;
    ss << "File: " << fileName << "\n";
    ss << "FileHash: " << fileHash << "\n";
    ss << "FileSize: " << fileIt->second->fileSize << "\n";
    ss << "ChunkHashes:";
    for (const auto& chunk : fileIt->second->chunks) {
        ss << " " << chunk.chunkHash;
    }
    ss << "\nSeeders:\n";
    
    for (const auto& [seederUsername, relativePath] : fileIt->second->seeders) {
        auto userIt = users.find(seederUsername);
        if (userIt != users.end() && userIt->second->isLoggedIn) {
            ss << seederUsername << " " << userIt->second->ip << " " << userIt->second->port << " " << relativePath << "\n";
        }
    }
    ss << "PartialSeeders:\n";
    for (const auto& [seederUsername, chunks] : fileIt->second->partialSeeders) {
        auto userIt = users.find(seederUsername);
        if (userIt != users.end() && userIt->second->isLoggedIn) {
            ss << seederUsername << " " << userIt->second->ip << " " << userIt->second->port;
            for (int chunkIndex : chunks) {
                ss << " " << chunkIndex;
            }
            ss << "\n";
        }
    }
    
    return ss.str();
}

string listGroups() {
    unique_lock<mutex> lock(groupsMutex);
    string result = "\nGroups: \n";
    for (const auto& group : groups) {
        result += group.first + "\n";
    }
    log("Listed all groups");
    return result;
}

string uploadFile(const string& username, const string& fileName, const string& groupName, size_t fileSize, const string& fileHash, const vector<string>& chunkHashes, const string& relativePath) {
    log("Attempt to upload file: " + fileName + " by user: " + username + " to group: " + groupName);
    const size_t CHUNK_SIZE = 512 * 1024;
    size_t expectedChunks = (fileSize + CHUNK_SIZE - 1) / CHUNK_SIZE;
    {
        unique_lock<mutex> lockGroups(groupsMutex);
        auto groupIt = groups.find(groupName);
        if (groupIt == groups.end()) {
            log("Upload failed: Group not found - " + groupName);
            return "Group not found";
        }
        if (groupIt->second->members.find(username) == groupIt->second->members.end()) {
            log("Upload failed: User " + username + " not a member of group " + groupName);
            return "You are not a member of this group";
        }
        groupIt->second->fileHashes.insert(fileHash);
        log("File hash added to group: " + groupName);
    }
    {
        unique_lock<mutex> lockFiles(filesMutex);
        auto fileIt = files.find(fileHash);
        if (fileIt == files.end()) {
            log("New file entry created: " + fileName);
            vector<Chunk> chunks;
            chunks.reserve(chunkHashes.size());
            for (const auto& chunkHash : chunkHashes) {
                chunks.push_back(Chunk{chunkHash, {username}});
            }
            files[fileHash] = make_shared<File>(File{fileName, fileHash, fileSize, chunks, {{username, relativePath}}, {}});
        } 
        else {
            log("Existing file updated: " + fileName);
            fileIt->second->seeders[username] = relativePath;
            for (auto& chunk : fileIt->second->chunks) {
                chunk.seeders.insert(username);
            }
            if (fileIt->second->fileSize != fileSize) {
                log("File size updated for: " + fileName);
                fileIt->second->fileSize = fileSize;
            }
        }
    }
    log("File uploaded successfully: " + fileName + " (Hash: " + fileHash + ") by " + username + " to group " + groupName);
    return "File uploaded successfully";
}

string listFiles(const string& username, const string& groupName) {
    unique_lock<mutex> lockGroups(groupsMutex);
    unique_lock<mutex> lockFiles(filesMutex);
    
    auto groupIt = groups.find(groupName);
    if (groupIt == groups.end()) {
        return "Group not found";
    }
    
    if (groupIt->second->members.find(username) == groupIt->second->members.end()) {
        return "You are not a member of this group";
    }
    
    string filesAvailable = "";
    for (const auto& fileHash : groupIt->second->fileHashes) {
        auto fileIt = files.find(fileHash);
        if (fileIt != files.end()) {
            filesAvailable += fileIt->second->fileName + " (Hash: " + fileHash + ")\n";
        }
    }
    
    return filesAvailable.empty() ? "No files available in this group" : "Available Files:\n" + filesAvailable;
}

string updatePartialSeederInfo(const string& username, const string& fileHash, int chunkIndex) {
    unique_lock<mutex> lockFiles(filesMutex);
    auto fileIt = files.find(fileHash);
    if (fileIt == files.end()) {
        return "File not found";
    }
    
    fileIt->second->partialSeeders[username].push_back(chunkIndex);
    
    bool isFullSeeder = fileIt->second->partialSeeders[username].size() == fileIt->second->chunks.size();
    if (isFullSeeder) {
        fileIt->second->seeders[username] = "";
        fileIt->second->partialSeeders.erase(username);
        return "User is now a full seeder";
    }
    
    return "Partial seeder info updated successfully";
}

string stopSharing(const string& groupName, const string& fileName, const string& username) {
    unique_lock<mutex> lockGroups(groupsMutex);
    unique_lock<mutex> lockFiles(filesMutex);
    
    auto groupIt = groups.find(groupName);
    if (groupIt == groups.end()) {
        return "Group not found";
    }
    
    string fileHash;
    for (const auto& hash : groupIt->second->fileHashes) {
        auto fileIt = files.find(hash);
        if (fileIt != files.end() && fileIt->second->fileName == fileName) {
            fileHash = hash;
            break;
        }
    }
    
    if (fileHash.empty()) {
        return "File not found in the group";
    }
    
    auto fileIt = files.find(fileHash);
    if (fileIt == files.end()) {
        return "File information not found";
    }
    
    fileIt->second->seeders.erase(username);
    fileIt->second->partialSeeders.erase(username);
    
    for (auto& chunk : fileIt->second->chunks) {
        chunk.seeders.erase(username);
    }
    
    if (fileIt->second->seeders.empty() && fileIt->second->partialSeeders.empty()) {
        groupIt->second->fileHashes.erase(fileHash);
        files.erase(fileIt);
        return "File removed from group as there are no seeders left";
    }
    
    return "Stopped sharing the file successfully";
}

string handleNewSeeder(const string& username, const string& groupName, const string& fileName, const string& fileHash) {
    lock_guard<mutex> lockFiles(filesMutex);
    lock_guard<mutex> lockGroups(groupsMutex);

    auto groupIt = groups.find(groupName);
    if (groupIt == groups.end()) {
        return "Group not found";
    }

    if (groupIt->second->members.find(username) == groupIt->second->members.end()) {
        return "User is not a member of this group";
    }

    auto fileIt = files.find(fileHash);
    if (fileIt == files.end()) {
        return "File not found";
    }

    fileIt->second->seeders[username] = "shared_files/" + groupName;
    log("Added new seeder: " + username + " for file " + fileName + " in group " + groupName);
    return "New seeder added successfully";
}

void* handle_client(void* arg) {
    unique_ptr<ClientInfo> client(static_cast<ClientInfo*>(arg));
    char buff[1024*1024];

    log("New client connected: " + client->ip + ":" + client->port);

    while (!shouldExit.load()) {
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

        int bytesReceived = recv(client->socket, buff, sizeof(buff), 0);
        if (bytesReceived == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            log("recv failed for client: " + client->ip + ":" + client->port);
            break;
        }
        if (bytesReceived == 0) {
            log("Client disconnected: " + client->ip + ":" + client->port);
            break;
        }

        buff[bytesReceived] = '\0';
        string command = string(buff);

        log("Received command from " + client->ip + ":" + client->port + ": " + command);

        istringstream iss(command);
        string cmd;
        iss >> cmd;

        string response;
        
        if (cmd == "create_user") {
            string username, password;
            iss >> username >> password;
            if(!username.empty() && !password.empty())
                response = createUser(username, password);
            else
                response="username or password cannot be empty!";
        } 
        else if (cmd == "login") {
            string username, password;
            iss >> username >> password;
            response = login(username, password, client.get());
        }
        else if (cmd == "logout") {
            response = logout(client.get());
        } 
        else if (cmd == "create_group") {
            if (client->username.empty()) {
                response = "You must be logged in to create a group";
            } 
            else {
                string groupId;
                iss >> groupId;
                if(!groupId.empty())
                    response = createGroup(client->username, groupId);
                else
                    response="Group ID can't be empty!";
            }
        } 
        else if (cmd == "join_group") {
            if (client->username.empty()) {
                response = "You must be logged in to join a group";
            } 
            else {
                string groupId;
                iss >> groupId;
                if(!groupId.empty())
                    response = joinGroup(client->username, groupId);
                else
                    response="Group ID can't be empty!";
            }
        } 
        else if (cmd == "leave_group") {
            if (client->username.empty()) {
                response = "You must be logged in to leave a group";
            } 
            else {
                string groupId;
                iss >> groupId;
                if(!groupId.empty())
                    response = leaveGroup(client->username, groupId);
                else
                    response="Group ID can't be empty!";
            }
        } 
        else if (cmd == "list_requests") {
            if (client->username.empty()) {
                response = "You must be logged in to list requests";
            } 
            else {
                string groupId;
                iss >> groupId;
                if(!groupId.empty())
                    response = listRequests(client->username, groupId);
                else
                    response="Group ID can't be empty!";
            }
        } 
        else if (cmd == "accept_request") {
            if (client->username.empty()) {
                response = "You must be logged in to accept requests";
            } 
            else {
                string groupId, requestUsername;
                iss >> groupId >> requestUsername;
                if(!groupId.empty() && !requestUsername.empty())
                    response = acceptRequest(groupId, requestUsername);
                else
                    response="Group ID or requester username can't be empty!";
            }
        } 
        else if (cmd == "list_groups") {
            if (client->username.empty()) {
                response = "You must be logged in to list groups";
            } 
            else
                response = listGroups();
        }
        else if(cmd == "upload_file"){
            if (client->username.empty()) {
                response = "You must be logged in to upload files";
            } 
            else
            {
                vector<string> chunkHashes;
                string groupName, fileName, fs, fileHash, chunksHash, relativePath;
                iss >> groupName >> fileName >> fs >> fileHash >> chunksHash >> relativePath;
                string chunkHash;
                stringstream ss(chunksHash);
                size_t fileSize = stoi(fs);
                while(getline(ss, chunkHash, '+')){
                    if(!chunkHash.empty())
                        chunkHashes.push_back(chunkHash);
                }
                response = uploadFile(client->username, fileName, groupName, fileSize, fileHash, chunkHashes, relativePath);
            }
        }
        else if(cmd == "list_files"){
            if (client->username.empty()) {
                response = "You must be logged in to list files";
            }
            else {
                string groupName;
                iss >> groupName;
                if(!groupName.empty()) {
                    response = listFiles(client->username, groupName);
                }
                else {
                    response = "Group name cannot be empty!";
                }
            }
        }
        else if (cmd == "download_file") {
            if (client->username.empty()) {
                response = "You must be logged in to download files";
            }
            else {
                string groupName, fileName;
                iss >> groupName >> fileName;
                if (!groupName.empty() && !fileName.empty()) {
                    response = downloadFile(client->username, groupName, fileName);
                }
                else {
                    response = "Group name and file name cannot be empty!";
                }
            }
        }
        else if (cmd == "add_leecher") {
            if (client->username.empty()) {
                response = "You must be logged in to update chunk information";
            }
            else {
                string fileHash;
                int chunkIndex;
                iss >> fileHash >> chunkIndex;
                if (!fileHash.empty()) {
                    response = updatePartialSeederInfo(client->username, fileHash, chunkIndex);
                }
                else {
                    response = "File hash cannot be empty!";
                }
            }
        }
        else if (cmd == "new_seeder") {
            string groupName, fileName, fileHash;
            iss >> groupName >> fileName >> fileHash;
            response = handleNewSeeder(client->username, groupName, fileName, fileHash);
        }
        else if(cmd == "stop_share"){
            if(client->username.empty()){
                response="You are not logged in";
            }
            else{
                string groupName, fileName;
                iss >> groupName >> fileName;
                if(!groupName.empty() && !fileName.empty()) {
                    response = stopSharing(groupName, fileName, client->username);
                }
                else {
                    response = "Group name and file name cannot be empty!";
                }
            }
        }
        else {
            response = "Unrecognized Command!";
        }
        
        send(client->socket, response.c_str(), response.size(), 0);
        log("Sent response to " + client->ip + ":" + client->port + ": " + response);
    }

    if (!client->username.empty()) {
        logout(client.get());
    }

    {
        lock_guard<mutex> lock(cv_m);
        openFds.erase(client->socket);
    }
    close(client->socket);
    log("Closed connection for client: " + client->ip + ":" + client->port);
    return nullptr;
}

void cleanup() {
    for (int fd : openFds) {
        close(fd);
    }
    openFds.clear();
    for (auto& thread : client_threads) {
        pthread_join(thread, nullptr);
    }
    client_threads.clear();
    close(listenSockServer);
    log("All resources released, shutting down tracker");
}

void* listenForQuit(void* args) {
    string inp;
    while (!shouldExit.load()) {
        getline(cin, inp);
        if (inp == "quit") {
            shouldExit.store(true);
            cv.notify_all();
            break;
        }
    }
    return nullptr;
}

pair<string,int> parseTrackerFile(string filePath, int num_trackers) {
    char buff[50];
    int fd = open(filePath.c_str(), O_RDONLY);

    while (true) {
        int bytesRead = read(fd, buff, sizeof(buff) - 1);
        if (bytesRead <= 0) {
            break;
        }

        buff[bytesRead] = '\0';
        stringstream ss(buff);
        string line;
        int lineCount = 1;
        while (getline(ss, line)) {
            if (line.empty()) {
                cout << "No IP and Port Found at Line " << num_trackers << endl;
                close(fd);
                exit(0);
            }
            int s = line.find_first_of(' ');
            if (s != string::npos) {
                string ip = line.substr(0, s);
                string portStr = line.substr(s + 1);
                int port = stoi(portStr);
                if(lineCount == num_trackers){
                    close(fd);
                    return {ip, port};
                }
            }
            lineCount++;
        }
    }
    close(fd);
    return {};
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        cerr << "Need 2 Arguments =>> <TrackerInfo File Path> And <Valid Line Number>" << endl;
        return -1;
    }

    logFD = open("tracker.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (logFD == -1) {
        cerr << "Failed to open log file" << endl;
        return -1;
    }

    string filePath = argv[1];
    int numTrackers = stoi(argv[2]);

    pair<string, int> data = parseTrackerFile(filePath, numTrackers);
    if(data.first.empty()){
        cout << "No IP and Port Found at Line " << numTrackers << endl;
        return -1;
    }
    string ip = data.first;
    int port;
    try {
        port = data.second;
    } catch (exception &e) {
        cerr << "Invalid port number: " << e.what() << endl;
        return -1;
    }

    listenSockServer = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSockServer < 0) {
        cerr << "Socket Creation Failed!" << endl;
        return -1;
    }
    
    sockaddr_in hint;
    hint.sin_family = AF_INET;
    hint.sin_port = htons(port);
    hint.sin_addr.s_addr = inet_addr(ip.c_str());

    if (bind(listenSockServer, (sockaddr*)&hint, sizeof(hint)) < 0) {
        cerr << "Socket Binding With Port Failed" << endl;
        close(listenSockServer);
        return -1;
    }

    if (listen(listenSockServer, 128) < 0) {
        cerr << "Failed to listen at PORT: " << port << endl;
        close(listenSockServer);
        return -1;
    }

    cout << "Tracker is listening on " << ip << ":" << port << endl;
    log("Tracker started on " + ip + ":" + to_string(port));

    pthread_t exitThread;
    if (pthread_create(&exitThread, nullptr, listenForQuit, nullptr) != 0) {
        cerr << "Failed to create exit thread" << endl;
        close(listenSockServer);
        return -1;
    }

    while (!shouldExit.load()) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listenSockServer, &readfds);
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        int activity = select(listenSockServer + 1, &readfds, nullptr, nullptr, &timeout);
        if (activity < 0 && errno != EINTR) {
            log("Select error");
            continue;
        }
        if (activity == 0) {
            continue;
        }
        if (FD_ISSET(listenSockServer, &readfds)) {
            sockaddr_in client_addr;
            socklen_t client_addr_size = sizeof(client_addr);
            int clientFD = accept(listenSockServer, (sockaddr*)&client_addr, &client_addr_size);
            if (clientFD < 0) {
                log("Failed to accept client connection");
                continue;
            }
            openFds.insert(clientFD);
            char ipPortDetails[1024];
            int br = recv(clientFD, ipPortDetails, 1024, 0);
            string ippd = string(ipPortDetails, br);
            int p = ippd.find_first_of(':');
            string ipc = ippd.substr(0, p);
            string portc = ippd.substr(p+1);

            auto client = make_unique<ClientInfo>(ClientInfo{clientFD, ipc, portc, ""});

            pthread_t client_thread;
            if (pthread_create(&client_thread, nullptr, handle_client, client.release()) != 0) {
                log("Failed to create thread for client: " + ipc + ":" + portc);
                close(clientFD);
                openFds.erase(clientFD);
                continue;
            }
            client_threads.push_back(client_thread);
            log("New client thread created for: " + ipc + ":" + portc);
        }
    }
    cleanup();
    pthread_join(exitThread, nullptr);
    close(listenSockServer);
    log("Tracker shut down gracefully");
    return 0;
}
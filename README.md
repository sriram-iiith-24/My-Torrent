
# AOS Assignment-3 [Peer-to-Peer Distributed File Sharing System]

This project implements a Peer-to-Peer (P2P) file sharing system where clients can share, upload, and download files with each other within groups. The system includes a **client** and a **tracker** that manage file sharing, user groups, and the integrity of shared files.

## Project Files

- **client.cpp**: Implements the client-side logic for uploading, downloading, and sharing files.
- **tracker.cpp**: Manages user authentication, group management, and file metadata storage for sharing files between clients.

## Features

### Client Side
- **Uploading Files**: Clients can upload files to the tracker, which stores file metadata and notifies other clients in the group.
- **Downloading Files**: Clients can download files from other clients (seeders) in the group.
- **File Integrity Verification**: SHA1 hash of the total file and its chunks are computed to ensure that files are downloaded without corruption.
- **Chunk Management**: Files are split into chunks, and each chunk can be downloaded from different seeders.
- **Show Downloads**: Clients can view the status of files they have downloaded.

### Tracker Side
- **User Management**: Supports user creation, login, and logout functionalities.
- **Group Management**: Users can create, join, or leave groups. Only group owners can manage group requests.
- **File Management**: The tracker maintains metadata of files, including the seeders for each file and the chunks they possess.
- **Partial Seeder Support**: Seeders can share only a part of a file, and the tracker tracks which chunks each seeder has.

## Usage

### Running the Tracker

To start the tracker, provide the tracker information file and the line number in the file where the tracker’s IP and port are specified.

```bash
./tracker <tracker_info_file_path> <line_number>
```

### Running the Client

To run the client, provide the IP and listening port along with the tracker information file.

```bash
./client <IP:Listening_PORT> <tracker_info_file_path>
```

### Commands for Client in regard to file sharing

- **upload_file <group_name> <file_path>**: Uploads a file to the tracker for sharing in the specified group.
- **download_file <group_name> <file_name> <destination_path>**: Downloads a file from seeders in the group.
- **stop_share <group_name> <file_name>**: Stops sharing a file within the group.
- **show_downloads**: Displays the status of all downloads.

## File Structures

### FileInfo
- **fileName**: Name of the file.
- **fileSize**: Size of the file.
- **fileHash**: SHA1 hash of the entire file.
- **chunkHashes**: SHA1 hash of each chunk of the file.
- **relativePath**: Path where the file is stored.

### DownloadedFileInfo
- **fileName**: Name of the downloaded file.
- **location**: Path where the downloaded file is stored.
- **groupName**: Group to which the file belongs.
- **status**: Status of the download.

## Compilation

Ensure you have the necessary libraries installed, such as OpenSSL for SHA1 hashing.

To compile the client and tracker, run:

```bash
g++ client.cpp -o client -lssl -lcrypto -pthread
g++ tracker.cpp -o tracker -lpthread
```

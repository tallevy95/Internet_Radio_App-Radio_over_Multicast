Internet Radio Application

Description:

  This project implements an Internet Radio application that allows clients to connect to a server and listen to music from 
  different stations. Clients can request songs, change stations, and even upload new songs to the server to be added to the 
  playlist. The server can handle multiple clients simultaneously, and the music streaming is done at a constant bit rate to 
  ensure smooth playback.
  This project was an assignment as part of the course "Computer Networks Design Laboratory"

Client Control:

  The "Client Control" program (radio_control) handles the interaction with the server and user. It connects to the server 
  using TCP and communicates according to the Server-Client protocol. The client can request songs by entering station 
  numbers, change stations, and upload new songs. The program uses UDP to receive music data and utilizes the "play" program
  to play the songs in real-time.

Server:

  The server program (radio_server) listens for client connections on a specified TCP port. It supports multiple clients and
  maintains a simple database of stations and connected clients. Each station plays a single song, and the server loops the
  songs indefinitely. Clients can upload new songs, and the server dynamically adds them to the playlist.

Rate Control:

  To ensure smooth music streaming, both the client and server implement rate control. The server streams music to clients at 
  a constant rate of 1KiB every 62500 usec. On the client side, music upload to the server occurs at a rate of 1KiB every 
  8000 usec.

Instructions:

  To run the application, follow these steps:
  
  1. Compile the server program: gcc -o radio_server radio_server.c -lpthread
  2. Compile the client control program: gcc -o radio_control radio_control.c
  3. Run the server: ./radio_server <tcpport> <mulitcastip> <udpport> <file1> <file2> ...
    <tcpport>: Port number for TCP connections.
    <mulitcastip>: IP address for multicast group station 0.
    <udpport>: Port number for UDP music streaming.
    <file1> <file2> ...: List of music files to be played by each station.
  4. Run the client control: ./radio_control <servername> <serverport>
    <servername>: IP address or hostname of the server.
    <serverport>: Port number of the server.

Commands:

  - Type a station number and press Enter to request a song from that station.
  - Type 's' and press Enter to upload a new song to the server.
  - Type 'q' and press Enter to quit the client program.

Note:

  - The server and client programs use Berkeley sockets API and are designed to work on laboratory computers with the specified
    multicast topology (GNS3) and virtual box machines (pc1, ..., pc4).
  - The server supports up to 100 clients simultaneously.
  - The server prints replies and commands to stdout for monitoring purposes.
  - Proper error handling is implemented to gracefully terminate connections in case of invalid commands or misbehaving
    clients.


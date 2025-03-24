#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/select.h>
#include <pthread.h>

#define SIZE 1024
#define MAX_CLIENTS 100
#define KIB 1024
#define TIME_INT 62500

//----------- STATE DEFINITIONS (main loop) -----------
// These define the states for the server's main menu.
#define SERVER_MAIN_MENU  0
#define SERVER_PRINT_INFO 1
#define SERVER_EXIT       2

//----------- STATE DEFINITIONS (manage_clients_control) -----------
// These define the states for each client thread as it handles
// messages (HELLO, ASK_SONG, UPSONG, etc.).
#define STATE_CONN_ESTABLISHED 0
#define STATE_ANNOUNCE         1
#define STATE_PERMIT           2
#define STATE_INVALID_CMD      3
#define STATE_NEWSTATIONS      4
#define STATE_WRONG_MSG        5
#define STATE_ERROR            6

//--------- TYPE OF SERVER MESSAGES -------
// These structures reflect the format of server<->client messages.
// For example, the client expects a 'Welcome' struct after it sends HELLO.
struct Welcome {
    uint8_t  replyType;          // 0 => WELCOME
    uint16_t numStations;        // Number of stations currently available
    uint8_t  multicastGroup[4];  // Base Multicast Group IP
    uint16_t portNumber;         // UDP port for the multicast stream
};

struct Announce {
    uint8_t  replyType;   // 1 => ANNOUNCE
    uint8_t  songNameSize;
    char*    songName;    // Name of the currently playing song
};

struct PermitSong {
    uint8_t  replyType;   // 2 => PERMIT
    uint8_t  permit;      // 0 = Not permitted, 1 = Permitted
};

struct InvalidCommand {
    uint8_t  replyType;       // 3 => INVALID_COMMAND
    uint8_t  replyStringSize; // Length of the error message
    char*    replyString;     // Error message text
};

struct NewStations {
    uint8_t  replyType;       // 4 => NEWSTATIONS
    uint16_t newStationNumber;// Total station count, after a new one is added
};

// Represents a station’s IP and the name of the song being looped.
struct Station {
    uint8_t  multicastGroup[4]; // e.g. 239.x.y.z
    uint8_t  songNameSize;      // length of the song name
    char*    songName;          // the actual song filename
};

// Holds information about an incoming song upload.
struct UpSong {
    uint32_t songSize;       // size in bytes of the song file
    uint8_t  songNameSize;   // length of the song’s filename
    char*    songName;       // filename (e.g. “track.mp3”)
};

//--------- GLOBAL VARS -------------
//
// Explanation of global variables and their usage:
//
// 1) 'control' is not used in the snippet, but presumably was intended for a separate
//    main-menu thread if needed.
// 2) 'client_list' stores references to up to 100 threads, each dedicated to handling
//    a single connected client.
// 3) 'station_list' is an array of threads, each broadcasting one of the known songs.
//    This list is dynamically allocated based on how many station files you initially pass in.
// 4) 'mutex_up' is a mutex protecting 'someone_sending' so only one client can upload
//    at a time without collision.
// 5) 'station_names' is a dynamic array storing information about each station
//    (multicast IP and the current song name).
// 6) 'welcome' is the server’s “WELCOME” message template. It’s updated as new stations are added.
// 7) 'flags' is an array controlling certain states in 'play_song' threads:
//    flags[1] => used to signal readiness of a station thread
//    flags[2] => used to signal if station threads should keep looping songs
// 8) 'num_clients' tracks how many clients are currently connected.
// 9) 'client_socket' stores up to 100 client sockets, parallel to 'client_list' threads.
// 10) 'global_running' is used to tell all client threads to keep going or terminate
//     (when the server is shutting down).
// 11) 'someone_sending' is set to 1 when a client is uploading a new .mp3, so
//     no other clients can do so simultaneously.

pthread_t control;                         
pthread_t client_list[MAX_CLIENTS] = {'\0'};
pthread_t *station_list;                   
pthread_mutex_t mutex_up;
struct Station* station_names;
struct Welcome welcome;

int flags[3] = {0, 0, 1};
int num_clients = 0;
int client_socket[MAX_CLIENTS] = {0};
int global_running = 1;
int someone_sending = 0;

//--------- FUNCTION PROTOTYPES --------------
int  OpenWelcomeSocket(uint16_t port);
void IP_Check(int station_index);
void* manage_clients_control(void* tcp_socket_index);
void* play_song(void* stationNum);

/**
 * main()
 * ------
 * The server program entry point. It:
 *   1) Parses command line arguments (TCP port, multicast IP, UDP port, .mp3 files).
 *   2) Allocates station threads to loop each .mp3 file as a separate station.
 *   3) Opens a TCP “welcome” socket.
 *   4) Enters a main loop for the server console, letting you see station/client info,
 *      and optionally exit.
 */
int main(int argc, char *argv[]) { 
    // Usage: <tcpPort> <multicastIP> <udpPort> <file1.mp3> <file2.mp3> ...
    int   Welcome_socket;
    int   select_num, recv_data;
    char  input[SIZE];
    int   state = SERVER_MAIN_MENU;
    int   end_run = 1; // flag to end the main server loop

    // We use these below for station creation
    int   i;
    char* substring = ".mp3";
    char* ptr;

    struct sockaddr_in welcomeAddr;
    struct sockaddr_storage serverStorage;
    socklen_t client_size, addr_size;
    fd_set fdset;

    // Initialize the mutex used for controlling uploads
    if(pthread_mutex_init(&mutex_up, NULL) != 0) {
        perror("Mutex init has failed\n");
        exit(1);
    }

    // Check we have at least 4 arguments: port, IP, port, and 1 file
    if(argc < 5) {
        printf("Invalid input!\n");
        printf("Usage: %s <tcpPort> <multicastIP> <udpPort> <file1.mp3> [file2.mp3] ...\n", argv[0]);
        exit(1);
    }

    // Parse CLI arguments:
    //  1) tcp_port   => the TCP socket for incoming client connections
    //  2) multicastIP => the base IP address for all stations
    //  3) udp_port   => the UDP port for the multicast group
    uint16_t tcp_port =  (uint16_t) strtol(argv[1], NULL, 10);
    welcome.portNumber = (uint16_t) strtol(argv[3], NULL, 10);
    inet_pton(AF_INET, argv[2], welcome.multicastGroup); 
    welcome.numStations = (uint16_t)(argc - 4); // number of .mp3 files
    welcome.replyType = 0;                      // 'WELCOME' message type

    // If we have station files, create threads for each station
    if(welcome.numStations > 0) {
        int num_files = (int)welcome.numStations;
        // Allocate arrays to hold station threads and station info
        station_list = (pthread_t*)malloc(sizeof(pthread_t)*num_files);
        station_names = (struct Station*)malloc(sizeof(struct Station)*num_files);

        if(!station_list || !station_names) {
            free(station_list);
            free(station_names);
            perror("malloc failed");
            exit(1);
        }
        
        // Loop over each file, set up station data, and start its thread
        for(i = 0; i < num_files; i++) {
            int station_index = i;
            inet_pton(AF_INET, argv[2], station_names[i].multicastGroup);

            // Adjust the IP address if the last octet overflows
            IP_Check(i);

            // Store the station’s file name
            station_names[i].songNameSize = (uint8_t)sizeof(argv[i + 4]);
            station_names[i].songName = (char*)malloc(sizeof(argv[i + 4]));
            strcpy(station_names[i].songName, argv[i + 4]);

            // flags[1] = 0 => we wait for the station thread to set it to 1 when it’s ready
            flags[1] = 0; 
            pthread_create(&station_list[i], NULL, &play_song, (void*)&station_index);

            // Busy-wait until the station thread signals readiness
            while(!flags[1]) { /* spin */ }
        }
    }

    // Open the main TCP “welcome” socket to accept new client connections
    Welcome_socket = OpenWelcomeSocket(tcp_port);
    if(Welcome_socket < 0) {
        close(Welcome_socket);
        exit(EXIT_FAILURE);
    }

    FD_ZERO(&fdset);
    FD_SET(Welcome_socket, &fdset);
    FD_SET(fileno(stdin), &fdset);

    // Present a basic console menu
    printf("Welcome to the Radio Server!\n");
    printf("Choose your next move:\n\t1 - See all current Stations & Clients\n\t2 - Exit\n");

    // Main server loop to handle new clients or console commands
    while(end_run) {
        switch(state) {

        //------------------------------------------------------------------
        // SERVER_MAIN_MENU: Wait for either user input on stdin or
        //                   a new client connecting to Welcome_socket.
        //------------------------------------------------------------------
        case SERVER_MAIN_MENU: {
            memset(input, '\0', sizeof(input));
            FD_SET(Welcome_socket, &fdset);
            FD_SET(fileno(stdin), &fdset);

            // Blocking select for either new connection or user input
            int select_num = select(FD_SETSIZE, &fdset, NULL, NULL, NULL);
            if(select_num < 0) {
                perror("Select - SERVER_MAIN_MENU - Failed");
                state = SERVER_EXIT;
                break;
            }
            // If user typed something on the console
            if(FD_ISSET(0, &fdset)) {
                int recv_data = (int)read(0, input, SIZE);
                if(recv_data < 0) {
                    perror("Reading - SERVER_MAIN_MENU - Failed");
                    state = SERVER_EXIT;
                    break;
                }
                // '1' => PRINT_INFO, '2' => EXIT
                state = input[0] - '0';
                if(state != SERVER_PRINT_INFO && state != SERVER_EXIT) {
                    printf("Wrong input, please enter again\n");
                    state = SERVER_MAIN_MENU;
                    printf("Choose your next move:\n"
                           "\t1 - See all current Stations & Clients\n"
                           "\t2 - Exit\n");
                }
            }
            // If a new client is connecting
            else if(FD_ISSET(Welcome_socket, &fdset)) {
                // Find an open slot in client_socket
                int index = 0;
                while(index < MAX_CLIENTS && client_socket[index] != 0) {
                    index++;
                }
                if(index == MAX_CLIENTS) {
                    printf("No more room for clients, try again later\n");
                    state = SERVER_MAIN_MENU;
                    break;
                }

                // Accept the incoming connection
                socklen_t client_size = sizeof(serverStorage);
                memset(&client_socket[index], 0, sizeof(client_socket[index]));
                client_socket[index] = accept(Welcome_socket,
                                             (struct sockaddr*)&serverStorage,
                                             &client_size);
                if(client_socket[index] < 0) {
                    perror("Accept - main - Failed");
                    state = SERVER_EXIT;
                    break;
                }
                // Confirm new client
                printf("***New client joined the server!***\n");

                // Create a thread to handle this client’s messages
                pthread_create(&client_list[index], NULL,
                               &manage_clients_control, (void*)&index);

                // Increase global count of active clients
                num_clients++;
                FD_SET(Welcome_socket, &fdset);
            }
        } break;

        //------------------------------------------------------------------
        // SERVER_PRINT_INFO: Display station and client info to console.
        //------------------------------------------------------------------
        case SERVER_PRINT_INFO: {
            // Print station info
            if(welcome.numStations == 0) {
                printf("We don't have any stations yet. Try again later!\n");
            } else {
                printf("We have %u stations!\n", welcome.numStations);
                for(i = 0; i < welcome.numStations; i++) {
                    printf("Station %d : %d.%d.%d.%d\n\t",
                           i,
                           station_names[i].multicastGroup[0],
                           station_names[i].multicastGroup[1],
                           station_names[i].multicastGroup[2],
                           station_names[i].multicastGroup[3]);
                    printf("Song name: %s\n", station_names[i].songName);
                }
            }

            // Print client info
            if(num_clients == 0) {
                printf("We don't have any clients yet. Try again later!\n");
            } else {
                printf("We have %u clients!\n", num_clients);
                int temp_clients = 1;
                for(i = 0; i < MAX_CLIENTS; i++) {
                    if(client_socket[i] != 0) {
                        // Retrieve the IP address of the connected client
                        struct sockaddr_storage ss;
                        socklen_t css = sizeof(ss);
                        getpeername(client_socket[i], (struct sockaddr*)&ss, &css);

                        char ip_address[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET,
                                  &((struct sockaddr_in*)&ss)->sin_addr,
                                  ip_address, INET_ADDRSTRLEN);

                        printf("Client %d with IP %s\n", temp_clients, ip_address);
                        temp_clients++;
                    }
                }
            }
            // Return to main menu prompt
            printf("Choose your next move:\n"
                   "\t1 - See all current Stations & Clients\n"
                   "\t2 - Exit\n");
            state = SERVER_MAIN_MENU;
        } break;

        //------------------------------------------------------------------
        // SERVER_EXIT: Shut down station threads, client threads, and
        //              close the welcome socket.
        //------------------------------------------------------------------
        case SERVER_EXIT: {
            // Tell all station threads to stop looping the songs
            flags[2] = 0;
            // Join them
            for(i = 0; i < welcome.numStations; i++) {
                free(station_names[i].songName);
                pthread_join(station_list[i], NULL);
            }
            // Tell all client threads to stop
            global_running = 0;
            for(i = 0; i < MAX_CLIENTS; i++) {
                if(client_list[i] != 0) {
                    pthread_join(client_list[i], NULL);
                }
            }
            // Free the arrays used by stations
            free(station_list);
            free(station_names);

            // Close the listening socket
            close(Welcome_socket);
            printf("Bye Bye\n");
            end_run = 0;
        } break;
        }
    }

    // Cleanup the mutex at the very end
    if(pthread_mutex_destroy(&mutex_up) != 0) {
        perror("MUTEX DESTROY FAILED");
        exit(-1);
    }
    return 1;
}

/**
 * IP_Check()
 * ----------
 * Adjusts the IP for station index 'station_index' so that each station
 * gets a unique multicast address. If adding 'station_index' to the
 * last octet overflows beyond 255, we increment the next octet, etc.
 *
 * For example, if the base IP is 239.0.0.255 and station_index is 1,
 * this function ensures the resulting IP is 239.0.1.0 (carrying over).
 */
void IP_Check(int station_index) {
    if((station_names[station_index].multicastGroup[3] + station_index) > 255) {
        int carry = (station_names[station_index].multicastGroup[3] + station_index) - 255;
        if(station_names[station_index].multicastGroup[2] == 255) {
            station_names[station_index].multicastGroup[2] = 0;
            station_names[station_index].multicastGroup[1] = welcome.multicastGroup[1] + 1;
        } else {
            station_names[station_index].multicastGroup[2] = welcome.multicastGroup[2] + 1;
        }
        station_names[station_index].multicastGroup[3] = (carry - 1);
    } else {
        station_names[station_index].multicastGroup[3] = welcome.multicastGroup[3] + station_index;
        station_names[station_index].multicastGroup[2] = welcome.multicastGroup[2];
    }
}

/**
 * play_song()
 * -----------
 * Each station has a dedicated thread created in main(), which repeatedly
 * loops over its assigned .mp3 file. The station:
 *   1) Creates a UDP socket.
 *   2) Sends datagrams containing chunks of the file at TIME_INT intervals
 *      to the station’s multicast address.
 *   3) Rewinds the file when it reaches the end, effectively looping forever
 *      until flags[2] is cleared.
 */
void* play_song(void* stationNum) {
    int s = *((int*)stationNum);  // station index
    int sockfd;
    struct sockaddr_in mcast_addr;
    char buffer[SIZE];
    unsigned char multi_ip[15];
    int ttl = 100;

    memset(buffer, '\0', SIZE);

    // Create a UDP socket for broadcasting
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if(sockfd < 0) {
        perror("socket - play_song - failed");
        exit(1);
    }

    // Construct the IP string, e.g. "239.x.y.z"
    sprintf((char*)multi_ip, "%d.%d.%d.%d",
            station_names[s].multicastGroup[0],
            station_names[s].multicastGroup[1],
            station_names[s].multicastGroup[2],
            station_names[s].multicastGroup[3]);

    mcast_addr.sin_family      = AF_INET;
    mcast_addr.sin_addr.s_addr = inet_addr((char*)multi_ip);
    mcast_addr.sin_port        = htons(welcome.portNumber);
    memset(mcast_addr.sin_zero, '\0', sizeof(mcast_addr.sin_zero));

    // Set the multicast TTL (Time-To-Live)
    if(setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {
        perror("setsockopt - IP_MULTICAST_TTL");
        exit(1);
    }

    // Open the file that will be repeatedly broadcast
    FILE *fp = fopen(station_names[s].songName, "r");
    if(fp == NULL) {
        perror("fopen - play_song - failed");
        exit(1);
    }

    // Signal that this station thread is now ready
    // (main thread watches flags[1] to ensure it’s safe to proceed)
    flags[1] = 1;

    // Loop as long as flags[2] is non-zero
    while(flags[2]) {
        // Read the file in KIB chunks (1 KiB at a time)
        // and send to the multicast group
        while(!feof(fp) && flags[2]) {
            int num_byte = (int)fread(buffer, 1, KIB, fp);
            sendto(sockfd, buffer, num_byte, 0, 
                   (struct sockaddr*)&mcast_addr, sizeof(mcast_addr));
            usleep(TIME_INT);
        }
        // Once we reach EOF, rewind the file to loop again
        rewind(fp);
    }

    // Clean up resources when the server is shutting down
    close(sockfd);
    fclose(fp);
    return NULL;
}

/**
 * manage_clients_control()
 * ------------------------
 * One thread per client. This function runs a loop receiving messages
 * from the client over TCP, parsing them, and responding. State transitions:
 *
 *  1) Wait for HELLO message.
 *     - If valid, send WELCOME.
 *     - If invalid, send INVALID_COMMAND or close.
 *
 *  2) Then read messages in a loop (ASK_SONG => ANNOUNCE, UPSONG => PERMIT, etc.).
 *     - If client closes or we get invalid data, we exit.
 *     - If a new song is uploaded, we send 'NewStations' to all clients.
 */
void* manage_clients_control(void* index_socket) {
    // Identify which client index we are handling
    int index = *((int*)index_socket);
    int sock = client_socket[index];

    // We begin in the "connection established" state,
    // expecting further messages.
    int state = STATE_CONN_ESTABLISHED;
    int select_num, recv_data, send_data;
    fd_set fdset1;
    unsigned char buffer[SIZE];

    // We’ll use these structures to build responses
    struct InvalidCommand Invalid;
    struct Announce       announce;
    struct UpSong         up_song;
    struct NewStations    new_stations;

    char* mp3 = ".mp3";
    char  mp3_check[5] = {'\0'};
    FILE* new_UpSong;

    // Timers used for waiting on certain messages
    struct timeval Hello_timeout, upsong_timeout;
    Hello_timeout.tv_sec  = 0;  Hello_timeout.tv_usec  = 300000; // 300 ms
    upsong_timeout.tv_sec = 3;  upsong_timeout.tv_usec = 0;

    memset(buffer, '\0', SIZE);

    //---- Wait for the initial HELLO message from the client ----
    FD_ZERO(&fdset1);
    FD_SET(sock, &fdset1);

    select_num = select(FD_SETSIZE, &fdset1, NULL, NULL, &Hello_timeout);
    if(select_num < 0) {
        perror("Select - manage_clients_control - Failed");
        close(sock);
        pthread_exit(&client_list[index]);
    } else if(select_num == 0) {
        // Timed out => client never sent HELLO
        printf("Hello timeout, closing connection...\n");
        close(sock);
        pthread_exit(&client_list[index]);
    } else if(FD_ISSET(sock, &fdset1)) {
        // Read the HELLO
        recv_data = (int)recv(sock, buffer, SIZE, 0);
        if(recv_data < 0) {
            perror("Reading - manage_clients_control - Failed");
            close(sock);
            pthread_exit(&client_list[index]);
        }

        // Validate that it's a proper 3-byte HELLO
        // (byte 0 => 0, bytes 1&2 => 0)
        if(buffer[0] != 0) {
            state = STATE_WRONG_MSG;
        }
        if(recv_data < 3) {
            state = STATE_INVALID_CMD;
            Invalid.replyType = 3;
            char msg[] = "Hello Message too short, Close connection";
            Invalid.replyStringSize = (uint8_t)strlen(msg);
            Invalid.replyString = (char*)malloc(Invalid.replyStringSize + 1);
            strcpy(Invalid.replyString, msg);
            printf("%s\n", Invalid.replyString);
        }
        if(recv_data > 3) {
            state = STATE_INVALID_CMD;
            Invalid.replyType = 3;
            char msg[] = "Hello Message too large, Close connection";
            Invalid.replyStringSize = (uint8_t)strlen(msg);
            Invalid.replyString = (char*)malloc(Invalid.replyStringSize + 1);
            strcpy(Invalid.replyString, msg);
            printf("%s\n", Invalid.replyString);
        }
        if(buffer[1] != 0 || buffer[2] != 0) {
            state = STATE_INVALID_CMD;
            Invalid.replyType = 3;
            char msg[] = "Wrong Hello Message, Close connection";
            Invalid.replyStringSize = (uint8_t)strlen(msg);
            Invalid.replyString = (char*)malloc(Invalid.replyStringSize + 1);
            strcpy(Invalid.replyString, msg);
            printf("%s\n", Invalid.replyString);
        }

        // If the HELLO is valid, immediately respond with WELCOME
        if(state == STATE_CONN_ESTABLISHED) {
            memset(buffer, '\0', SIZE);
            buffer[0] = welcome.replyType;  // 0 => WELCOME

            // Convert to network byte order
            uint16_t numstation_net = htons(welcome.numStations);
            memcpy(buffer + 1, &numstation_net, 2);

            // Copy IP data
            buffer[3] = welcome.multicastGroup[3];
            buffer[4] = welcome.multicastGroup[2];
            buffer[5] = welcome.multicastGroup[1];
            buffer[6] = welcome.multicastGroup[0];

            uint16_t portnumber_net = htons(welcome.portNumber);
            memcpy(buffer + 7, &portnumber_net, 2);

            // Send WELCOME
            send_data = (int)send(sock, buffer, 9, 0);
            if(send_data < 0) {
                perror("Sending welcome Failed");
                state = STATE_ERROR;
            }
        }
    }

    int local_running = 1; // loops until we hit STATE_ERROR or server shuts down
    struct timeval timeout;
    timeout.tv_sec  = 0;
    timeout.tv_usec = 50000; // 50 ms for select

    //---- Main loop to handle messages from this client ----
    while(local_running && global_running) {
        switch(state) {

        //-------------------------------------------------------------
        // STATE_CONN_ESTABLISHED
        //-------------------------------------------------------------
        case STATE_CONN_ESTABLISHED: {
            memset(buffer, '\0', SIZE);
            FD_ZERO(&fdset1);
            FD_SET(sock, &fdset1);
            timeout.tv_usec = 50000;

            // Wait for the next message
            int select_num = select(FD_SETSIZE, &fdset1, NULL, NULL, &timeout);
            if(select_num < 0) {
                perror("STATE_CONN_ESTABLISHED select failed");
                state = STATE_ERROR;
                break;
            }
            if(FD_ISSET(sock, &fdset1)) {
                // Attempt to read message from client
                recv_data = (int)recv(sock, buffer, SIZE, 0);
                if(recv_data < 0) {
                    perror("Reading - manage_clients_control - Failed");
                    close(sock);
                    pthread_exit(&client_list[index]);
                }
                // If 0 bytes => client disconnected
                if(recv_data == 0) {
                    printf("***Client left the server***\n");
                    state = STATE_ERROR; 
                    break;
                }

                // Switch on the first byte (message type)
                switch(buffer[0]) {
                case 0: { 
                    // Another HELLO => invalid command
                    state = STATE_INVALID_CMD;
                    Invalid.replyType = 3;
                    char msg[] = "Duplicate HELLO, Close connection";
                    Invalid.replyStringSize = (uint8_t)strlen(msg);
                    Invalid.replyString = (char*)malloc(Invalid.replyStringSize + 1);
                    strcpy(Invalid.replyString, msg);
                } break;

                case 1: { // ASK_SONG => we go to STATE_ANNOUNCE
                    // Validate length (should be exactly 3 bytes)
                    if(recv_data < 3 || recv_data > 3) {
                        state = STATE_INVALID_CMD;
                        Invalid.replyType = 3;
                        char msg[] = "AskSong Message invalid length, Close connection";
                        Invalid.replyStringSize = (uint8_t)strlen(msg);
                        Invalid.replyString = (char*)malloc(Invalid.replyStringSize+1);
                        strcpy(Invalid.replyString, msg);
                        printf("%s\n", Invalid.replyString);
                    } else {
                        state = STATE_ANNOUNCE;
                    }
                } break;

                case 2: { // UPSONG => go to STATE_PERMIT
                    state = STATE_PERMIT;
                } break;

                default: {
                    // Unrecognized => go to STATE_WRONG_MSG
                    state = STATE_WRONG_MSG;
                } break;
                }
            }
        } break;

        //-------------------------------------------------------------
        // STATE_ANNOUNCE (handle AskSong)
        //-------------------------------------------------------------
        case STATE_ANNOUNCE: {
            // The station number is in buffer[1..2]
            uint16_t station_request = (buffer[1] << 8);
            station_request += (uint16_t)buffer[2];

            // Validate station index
            if(station_request >= welcome.numStations) {
                state = STATE_INVALID_CMD;
                Invalid.replyType = 3;
                char msg[] = "Invalid station number, Close connection";
                Invalid.replyStringSize = (uint8_t)strlen(msg);
                Invalid.replyString = (char*)malloc(Invalid.replyStringSize + 1);
                strcpy(Invalid.replyString, msg);
                break;
            }

            // Build ANNOUNCE message
            announce.replyType    = 1;
            announce.songNameSize = station_names[station_request].songNameSize;
            announce.songName     = (char*)malloc(announce.songNameSize + 1);
            strcpy(announce.songName, station_names[station_request].songName);

            // Pack ANNOUNCE into buffer
            memset(buffer, '\0', SIZE);
            buffer[0] = announce.replyType;
            buffer[1] = announce.songNameSize;
            strcpy((char*)(buffer + 2), announce.songName);

            // Send it
            send_data = (int)send(sock, buffer, 2 + announce.songNameSize, 0);
            if(send_data < 0) {
                perror("STATE_ANNOUNCE: send(Announce) Failed");
                state = STATE_ERROR;
            }
            free(announce.songName);
            // Return to normal
            state = STATE_CONN_ESTABLISHED;
        } break;

        //-------------------------------------------------------------
        // STATE_PERMIT (handle UpSong)
        //-------------------------------------------------------------
        case STATE_PERMIT: {
            uint32_t songsize_upsong = 0;
            // Next 4 bytes after the type is the file size in network order
            memcpy(&songsize_upsong, buffer + 1, 4);
            songsize_upsong = ntohl(songsize_upsong);

            up_song.songSize     = songsize_upsong;
            up_song.songNameSize = buffer[5];

            // Validate message length
            if(recv_data < (6 + up_song.songNameSize)) {
                state = STATE_INVALID_CMD;
                Invalid.replyType = 3;
                char msg[] = "UpSong Message too short, Close connection";
                Invalid.replyStringSize = (uint8_t)strlen(msg);
                Invalid.replyString = (char*)malloc(Invalid.replyStringSize + 1);
                strcpy(Invalid.replyString, msg);
                printf("%s\n", Invalid.replyString);
                break;
            }
            if(recv_data > (6 + up_song.songNameSize)) {
                state = STATE_INVALID_CMD;
                Invalid.replyType = 3;
                char msg[] = "UpSong Message too large, Close connection";
                Invalid.replyStringSize = (uint8_t)strlen(msg);
                Invalid.replyString = (char*)malloc(Invalid.replyStringSize + 1);
                strcpy(Invalid.replyString, msg);
                printf("%s\n", Invalid.replyString);
                break;
            }

            // Copy the incoming filename
            up_song.songName = (char*)malloc(up_song.songNameSize + 1);
            strcpy(up_song.songName, (char*)(buffer + 6));
            up_song.songName[up_song.songNameSize] = '\0';

            // Check if it ends in ".mp3"
            int a = up_song.songNameSize - 4;
            strcpy(mp3_check, up_song.songName + a); 
            int permit_flag = 1; // assume permitted unless we see a reason not to
            if(strcmp(mp3, mp3_check)) {
                permit_flag = 0; // not .mp3
            }

            // Ensure we don’t duplicate an existing station name
            int i;
            for(i = 0; i < welcome.numStations; i++) {
                if(!strcmp(up_song.songName, station_names[i].songName)) {
                    permit_flag = 0;
                    break;
                }
            }
            // Check size boundaries
            if(up_song.songSize < 2000) {
                state = STATE_INVALID_CMD;
                Invalid.replyType = 3;
                char msg[] = "The song size is too small, Close connection";
                Invalid.replyStringSize = (uint8_t)strlen(msg);
                Invalid.replyString = (char*)malloc(Invalid.replyStringSize + 1);
                strcpy(Invalid.replyString, msg);
                break;
            }
            if(up_song.songSize > 10485760) {
                state = STATE_INVALID_CMD;
                Invalid.replyType = 3;
                char msg[] = "The song size is too large, Close connection";
                Invalid.replyStringSize = (uint8_t)strlen(msg);
                Invalid.replyString = (char*)malloc(Invalid.replyStringSize + 1);
                strcpy(Invalid.replyString, msg);
                break;
            }

            // If someone else is already uploading, we can’t allow it
            if(someone_sending) {
                permit_flag = 0;
            }

            // Attempt to lock the mutex to see if we can set 'someone_sending'
            if(pthread_mutex_trylock(&mutex_up) == 0) {
                if(!someone_sending) {
                    someone_sending = 1;
                }
                pthread_mutex_unlock(&mutex_up);
            } else {
                permit_flag = 0;
            }

            // Decide which permit message to send
            uint8_t permit0[2] = {2, 0}; // "Not permitted"
            uint8_t permit1[2] = {2, 1}; // "Permitted"

            // Send permit or not
            if(permit_flag) {
                send_data = (int)send(sock, permit1, 2, 0);
                if(send_data < 0) {
                    perror("STATE_PERMIT: sending permit=1 failed");
                    state = STATE_ERROR;
                    break;
                }
            } else {
                send_data = (int)send(sock, permit0, 2, 0);
                if(send_data < 0) {
                    perror("STATE_PERMIT: sending permit=0 failed");
                    state = STATE_ERROR;
                    break;
                }
                // Return to normal if we refuse the upload
                state = STATE_CONN_ESTABLISHED;
                free(up_song.songName);
                someone_sending = 0;
                break;
            }

            // Actually accept the file data
            FILE* new_UpSong = fopen(up_song.songName, "r");
            int flag_new = 0; // If 0 => file existed, 1 => new file
            if(new_UpSong == NULL) {
                new_UpSong = fopen(up_song.songName, "w");
                flag_new = 1;
            }

            int byte_sent = 0;
            printf("***New song arriving!***\n");

            // Read from the client in chunks until we get the entire file
            while(byte_sent < (int)up_song.songSize) {
                memset(buffer, '\0', SIZE);
                FD_ZERO(&fdset1);
                FD_SET(sock, &fdset1);

                select_num = select(FD_SETSIZE, &fdset1, NULL, NULL, &upsong_timeout);
                if(select_num < 0) {
                    perror("STATE_PERMIT: select for file data failed");
                    state = STATE_ERROR;
                    break;
                } else if(select_num == 0) {
                    // Timed out => invalid
                    printf("UpSong timeout, close connection\n");
                    state = STATE_INVALID_CMD;
                    Invalid.replyType = 3;
                    char msg[] = "UpSong timeout, Close connection";
                    Invalid.replyStringSize = (uint8_t)strlen(msg);
                    Invalid.replyString = (char*)malloc(Invalid.replyStringSize + 1);
                    strcpy(Invalid.replyString, msg);
                    break;
                } else if(FD_ISSET(sock, &fdset1)) {
                    recv_data = (int)recv(sock, buffer, KIB, 0);
                    if(recv_data < 0) {
                        perror("STATE_PERMIT: recv file data failed");
                        close(sock);
                        pthread_exit(&client_list[index]);
                    }
                    if(flag_new == 1) {
                        fwrite(buffer, 1, recv_data, new_UpSong);
                    }
                    byte_sent += recv_data;
                }
                upsong_timeout.tv_sec = 3;
                if(state != STATE_PERMIT) {
                    // If we changed state in the loop, bail out
                    break;
                }
            }
            fclose(new_UpSong);
            // Allow next upload
            someone_sending = 0;

            // If fully received and still in PERMIT => new station time
            if(byte_sent == (int)up_song.songSize && state == STATE_PERMIT) {
                state = STATE_NEWSTATIONS;
            }
        } break;

        //-------------------------------------------------------------
        // STATE_INVALID_CMD
        //-------------------------------------------------------------
        case STATE_INVALID_CMD: {
            // Build the 'InvalidCommand' message
            memset(buffer, '\0', SIZE);
            buffer[0] = Invalid.replyType; // 3 => invalid
            buffer[1] = Invalid.replyStringSize;
            strcpy((char*)(buffer + 2), Invalid.replyString);

            // Send to client
            send_data = (int)send(sock, buffer, 2 + Invalid.replyStringSize, 0);
            if(send_data < 0) {
                perror("STATE_INVALID_CMD: send(InvalidCommand) failed");
                state = STATE_ERROR;
                break;
            }
            free(Invalid.replyString);
            state = STATE_ERROR;
        } break;

        //-------------------------------------------------------------
        // STATE_NEWSTATIONS
        //-------------------------------------------------------------
        case STATE_NEWSTATIONS: {
            // Increase station count
            welcome.numStations++;
            // Reallocate station_names to hold an extra station
            station_names = (struct Station*)realloc(station_names,
                            sizeof(struct Station) * welcome.numStations);
            if(!station_names) {
                perror("realloc station_names failed");
                state = STATE_ERROR;
                break;
            }

            // The new station is at the end
            int i = welcome.numStations - 1;
            // Copy the base IP from welcome
            int j;
            for(j = 0; j < 4; j++) {
                station_names[i].multicastGroup[j] = welcome.multicastGroup[j];
            }
            // IP_CHeck to adjust if needed
            IP_Check(i);

            // Copy the new song name from 'up_song'
            station_names[i].songNameSize = up_song.songNameSize;
            station_names[i].songName = (char*)malloc(up_song.songNameSize + 1);
            strcpy(station_names[i].songName, up_song.songName);

            // Launch a new thread for the new station
            pthread_create(&station_list[i], NULL, &play_song, (void*)&i);

            // Build NEWSTATIONS message
            new_stations.replyType = 4; // '4' => NEWSTATIONS
            new_stations.newStationNumber = htons(welcome.numStations);

            memset(buffer, '\0', SIZE);
            buffer[0] = new_stations.replyType;
            memcpy(buffer + 1, &new_stations.newStationNumber, 2);

            // Send this update to ALL connected clients
            for(i = 0; i < MAX_CLIENTS; i++) {
                if(client_socket[i] != 0) {
                    send_data = (int)send(client_socket[i], buffer, 3, 0);
                    if(send_data < 0) {
                        perror("STATE_NEWSTATIONS: broadcast failed");
                        state = STATE_ERROR;
                    }
                }
            }
            // Clean up the up_song struct's allocated memory
            free(up_song.songName);
            state = STATE_CONN_ESTABLISHED;
        } break;

        //-------------------------------------------------------------
        // STATE_WRONG_MSG
        //-------------------------------------------------------------
        case STATE_WRONG_MSG: {
            printf("STATE_WRONG_MSG: unrecognized message\n");
            state = STATE_ERROR;
        } break;

        //-------------------------------------------------------------
        // STATE_ERROR
        //-------------------------------------------------------------
        case STATE_ERROR: {
            // We close the socket and mark this slot as free
            local_running = 0;
            close(sock);
            client_socket[index] = 0;
            num_clients--;
        } break;

        } // end switch
    }

    pthread_exit(&client_list[index]);
}

/**
 * OpenWelcomeSocket()
 * -------------------
 * Creates a TCP socket for listening on the specified port,
 * sets reuse options, binds, and calls listen().
 * Returns the socket descriptor on success, or -1 on failure.
 */
int OpenWelcomeSocket(uint16_t port) {
    int serverSocket, option = 1;
    struct sockaddr_in serverAddr;
    socklen_t server_size;

    // Create socket
    serverSocket = socket(PF_INET, SOCK_STREAM, 0);
    if(serverSocket < 0) {
        perror("Socket Failed");
        close(serverSocket);
        return -1;
    }

    // Enable re-using addresses/ports
    if(setsockopt(serverSocket, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT,
                  &option, sizeof(option))) {
        perror("Socket Option Failed");
        close(serverSocket);
        return -1;
    }

    // Prepare server address struct
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_port        = htons(port);
    serverAddr.sin_addr.s_addr = INADDR_ANY; // Accept on any local IP
    memset(serverAddr.sin_zero, '\0', sizeof(serverAddr.sin_zero));
    server_size = sizeof(serverAddr);

    // Bind to the specified port
    if(bind(serverSocket, (struct sockaddr*)&serverAddr, server_size) < 0) {
        perror("Bind Failed");
        close(serverSocket);
        return -1;
    }

    // Listen for incoming connections
    if(listen(serverSocket, SOMAXCONN) == 0) {
        // Optionally print "Listening..." if desired
    } else {
        perror("Listen Failed");
        close(serverSocket);
        return -1;
    }
    return serverSocket;
}

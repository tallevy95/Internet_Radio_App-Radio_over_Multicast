#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <linux/tcp.h>

#define SIZE 1024
#define TIME_INTERVAL 8000
#define KIB 1024
#define RECV_TIME 62500

//----------- STATE DEFINITIONS -----------
#define MAIN_MENU        0
#define ASK_SONG         1
#define UP_SONG          2
#define EXIT_PROGRAM     3
#define INVALID_COMMAND  4
#define ERROR_STATE      5

//--------- GLOBAL VARS -------------
pthread_t recv_song;        // Receives songs from the server via UDP 
pthread_t control;          // Handles user commands vs. the server
pthread_t send_song;        // Sends songs to the server via TCP (unused in snippet)
unsigned char multicast_group[4]; // Base multicast IP for station 0
uint16_t numStations, portNumber;
char* server_IP;            // IP of the server
int play_flag = 1;          // Tells the multicast thread whether to continue playing
int change_station = 0;     // Tells the multicast thread that a station change occurred
int wait = 1;               // Wait flag for IP change in the multicast thread
int flag_termniate = 0;     // Indicates if the server terminated the connection
unsigned char ip_addr[4];   // Current station’s multicast address

// Timers for various message timeouts
struct timeval welcome_time;     
struct timeval announce_time;   
struct timeval permit_time;     
struct timeval NewStation_time; 

//--------- FUNCTION PROTOTYPES --------------
void* UpSong(void* tcp_soc);    // (Unused; left for consistency if needed)
void EndFunc();                 // Closes all memory and threads (not fully used)
void* control_func(void *tcp_soc);
void* playsong(void *ip);

//--------------------------------------------
int main(int argc, char *argv[]) {
    int select_num;
    int tcp_port;          // TCP port
    int send_data = 0, recv_data = 0;
    int R_socket;          // Socket descriptor
    char buffer[SIZE];
    unsigned char hello_msg[3] = {'\0'}; // 3-byte HELLO message
    fd_set fdset;
    struct sockaddr_in serverAddr;
    socklen_t addr_size;
    struct timeval harta;
    uint8_t welcome_type;

    if (argc < 3) {
        printf("Usage: %s <ServerIP> <TCPPort>\n", argv[0]);
        return -1;
    }

    // Initialize timeouts
    welcome_time.tv_sec   = 0; welcome_time.tv_usec   = 300000; // 300 ms
    announce_time.tv_sec  = 0; announce_time.tv_usec  = 300000; // 300 ms
    permit_time.tv_sec    = 0; permit_time.tv_usec    = 300000; // 300 ms
    NewStation_time.tv_sec= 2; NewStation_time.tv_usec= 0;      // 2 seconds

    FD_ZERO(&fdset);
    memset(buffer, '\0', sizeof(buffer));

    // Parse TCP port from command line
    tcp_port = (short)strtol(argv[2], NULL, 10);

    // Create TCP socket
    R_socket = socket(PF_INET, SOCK_STREAM, 0);
    if (R_socket < 0) {
        perror("Socket Failed");
        close(R_socket);
        exit(EXIT_FAILURE);
    }

    printf("Connecting to server...\n");
    // Set properties for R_socket
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_port        = htons(tcp_port);
    serverAddr.sin_addr.s_addr = inet_addr(argv[1]);
    memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);

    // Connect the socket to the server
    addr_size = sizeof(serverAddr);
    if ((connect(R_socket, (struct sockaddr*) &serverAddr, addr_size)) < 0) {
        perror("Socket Connection Failed");
        close(R_socket);
        exit(EXIT_FAILURE);
    }
    printf("Socket Connection Succeed.\n");

    // Small timeout to see if the server sent anything prior to HELLO
    harta.tv_sec  = 0;
    harta.tv_usec = 10000;  // 10 ms
    FD_SET(R_socket, &fdset);
    select_num = select(FD_SETSIZE, &fdset, NULL, NULL, &harta);
    if (select_num < 0) {
        perror("Select Failed");
        close(R_socket);
        exit(EXIT_FAILURE);
    }
    // If something arrived before HELLO, read it and close
    if (select_num > 0 && FD_ISSET(R_socket, &fdset)) {
        recv_data = recv(R_socket, buffer, SIZE, 0);
        if (recv_data < 0) {
            perror("Reading Failed");
            close(R_socket);
            exit(EXIT_FAILURE);
        }
        if (recv_data > 0) {
            printf("Message arrived before HELLO; terminating connection.\n");
            close(R_socket);
            exit(1);
        }
    }

    // Send HELLO message
    send_data = send(R_socket, hello_msg, 3, 0);
    if (send_data < 0) {
        perror("Sending HELLO Failed");
        close(R_socket);
        exit(EXIT_FAILURE);
    }

    // Wait for WELCOME message
    FD_SET(R_socket, &fdset);
    FD_SET(fileno(stdin), &fdset);
    select_num = select(FD_SETSIZE, &fdset, NULL, NULL, &welcome_time);
    if (select_num < 0) {
        perror("Select Failed");
        close(R_socket);
        exit(EXIT_FAILURE);
    } else if (select_num == 0) {
        printf("Welcome timeout, closing connection...\n");
        close(R_socket);
        exit(EXIT_FAILURE);
    }
    // WELCOME arrived
    else if (FD_ISSET(R_socket, &fdset)) {
        recv_data = recv(R_socket, buffer, SIZE, 0);
        if (recv_data < 0) {
            perror("Reading Failed");
            close(R_socket);
            exit(EXIT_FAILURE);
        }
        welcome_type = buffer[0];

        // Validate WELCOME length
        if (recv_data < 9) {
            printf("Welcome message too short, closing connection...\n");
            close(R_socket);
            return 0;
        } else if (recv_data > 9 && buffer[9] != '\0') {
            printf("Welcome message too long, closing connection...\n");
            close(R_socket);
            return 0;
        }

        // Check the message type
        if (welcome_type == 0) {
            // Extract station count
            numStations = (buffer[1] << 8) + (unsigned char)buffer[2];
            // Extract IP address
            multicast_group[3] = buffer[3]; ip_addr[3] = buffer[3];
            multicast_group[2] = buffer[4]; ip_addr[2] = buffer[4];
            multicast_group[1] = buffer[5]; ip_addr[1] = buffer[5];
            multicast_group[0] = buffer[6]; ip_addr[0] = buffer[6];
            // Extract port
            portNumber = (buffer[7] << 8) + (unsigned char)buffer[8];

            printf("Welcome to Radio Server %s\n"
                   "Multicast address: %d.%d.%d.%d\n"
                   "Number of stations: %d\n"
                   "Port Number: %u\n",
                   argv[1],
                   ip_addr[0], ip_addr[1], ip_addr[2], ip_addr[3],
                   numStations, portNumber);

            // Launch the two main threads
            pthread_create(&control, NULL, &control_func, (void*)&R_socket);
            pthread_create(&recv_song, NULL, &playsong, (void*)ip_addr);
        }
        // If invalid command arrived
        else if (welcome_type == 3) {
            int size_invalid = (unsigned int)buffer[1];
            char* replyString = (char*)malloc(size_invalid + 1);
            replyString[size_invalid] = '\0';
            strcpy(replyString, buffer + 2);
            printf("%s\n", replyString);
            free(replyString);
            close(R_socket);
            return 0;
        } else {
            printf("Unknown message from server, closing connection...\n");
            close(R_socket);
            return 0;
        }
    }

    // Wait for the control thread to exit, then close socket
    pthread_join(control, NULL);
    close(R_socket);
    return 0;
}

/**
 * Thread function that manages all user input (stdin) and interactions
 * with the server over the TCP socket.
 */
void* control_func(void *tcp_soc) {
    int tcp_socket = *((int*)tcp_soc);
    int state = MAIN_MENU;
    int user_station;
    int select_num = 0, recv_data = 0, send_data = 0;
    int sum_of_bytes = 0;
    double p_song_sent = 0.0;
    uint8_t Ask_song[3] = {1, 0, 0};
    uint8_t Announce_type;
    uint16_t temp_station;
    char buffer[SIZE], input[SIZE];
    int song_size;
    char *song_name;
    char up_song_name[255] = {'\0'}; // name for upload
    uint8_t upsong_name_size;
    uint32_t upsong_size;
    fd_set fdset1;
    FILE* upload_song;

    memset(buffer, '\0', SIZE);
    memset(input, '\0', SIZE);

    while (1) {
        switch (state) {

        //---------------------------------------------
        // MAIN_MENU
        //---------------------------------------------
        case MAIN_MENU: {
            FD_ZERO(&fdset1);
            FD_SET(fileno(stdin), &fdset1);
            FD_SET(tcp_socket, &fdset1);

            printf("Enter:\n\t1- to AskSong\n\t2- to UpSong\n\t3- to Exit\n");

            select_num = select(FD_SETSIZE, &fdset1, NULL, NULL, NULL);
            if (select_num < 0) {
                perror("Select Failed");
                state = ERROR_STATE;
                break;
            }
            // Keyboard input
            if (FD_ISSET(0, &fdset1)) {
                memset(input, '\0', SIZE);
                recv_data = read(0, input, SIZE);
                if (recv_data < 0) {
                    perror("Reading stdin Failed");
                    state = ERROR_STATE;
                    break;
                }
                state = input[0] - '0'; // 1->ASK_SONG, 2->UP_SONG, 3->EXIT
                if (state != ASK_SONG && state != UP_SONG && state != EXIT_PROGRAM) {
                    printf("Wrong input, please enter again\n");
                    state = MAIN_MENU;
                }
            }
            // Server message arrived unexpectedly
            else if (FD_ISSET(tcp_socket, &fdset1)) {
                memset(buffer, '\0', SIZE);
                recv_data = read(tcp_socket, buffer, SIZE);
                if (recv_data < 0) {
                    perror("Reading Failed");
                    state = ERROR_STATE;
                    break;
                }
                // Server closed connection
                if (recv_data == 0) {
                    printf("Server terminated connection\n");
                    state = EXIT_PROGRAM;
                    flag_termniate = 1;
                    break;
                }
                // NewStations
                if (buffer[0] == 4) {
                    if (recv_data < 3) {
                        printf("NewStations message too short, closing...\n");
                        state = EXIT_PROGRAM;
                        break;
                    } else if (recv_data > 3 && buffer[3] != '\0') {
                        printf("NewStations message too long, closing...\n");
                        state = EXIT_PROGRAM;
                        break;
                    }
                    numStations = (buffer[1] << 8) + (unsigned char)buffer[2];
                    printf("NewStations arrived, we have %u stations.\n", numStations);
                }
                // Invalid Command
                else if (buffer[0] == 3) {
                    state = INVALID_COMMAND;
                }
                // Something else unexpected
                else {
                    state = ERROR_STATE;
                }
            }
        } break;

        //---------------------------------------------
        // ASK_SONG
        //---------------------------------------------
        case ASK_SONG: {
            printf("Enter station number: (0 to %u)\n", numStations - 1);
            FD_ZERO(&fdset1);
            FD_SET(fileno(stdin), &fdset1);
            select_num = select(1, &fdset1, NULL, NULL, NULL);
            if (select_num < 0) {
                perror("Select AskSong Failed");
                state = ERROR_STATE;
                break;
            }

            if (FD_ISSET(0, &fdset1)) {
                memset(input, '\0', SIZE);
                recv_data = read(0, input, SIZE);
                if (recv_data < 0) {
                    perror("Reading AskSong Failed");
                    state = ERROR_STATE;
                    break;
                }
            }

            user_station = input[0] - '0'; // simplistic parse
            if (user_station < 0 || user_station > (numStations - 1)) {
                printf("Wrong station input, please enter again\n");
                state = ASK_SONG;
                break;
            }

            // Build the AskSong packet
            temp_station   = (uint16_t)user_station;
            Ask_song[1]   = (uint8_t)(temp_station >> 8);
            Ask_song[2]   = (uint8_t)(temp_station & 0x00FF);
            send_data     = send(tcp_socket, Ask_song, 3, 0);
            if (send_data < 0) {
                perror("Sending AskSong Failed");
                state = ERROR_STATE;
                break;
            }

            // Wait for the Announce message
            FD_ZERO(&fdset1);
            FD_SET(tcp_socket, &fdset1);
            select_num = select(FD_SETSIZE, &fdset1, NULL, NULL, &announce_time);
            announce_time.tv_usec = 300000; // reset

            memset(buffer, '\0', SIZE);
            if (select_num < 0) {
                perror("Select AskSong Failed 2");
                state = ERROR_STATE;
                break;
            } else if (select_num == 0) {
                printf("Timeout in Announce message, terminate...\n");
                state = ERROR_STATE;
                break;
            } else if (FD_ISSET(tcp_socket, &fdset1)) {
                recv_data = recv(tcp_socket, buffer, SIZE, 0);
                if (recv_data < 0) {
                    perror("Reading Failed");
                    state = ERROR_STATE;
                    break;
                }
            }

            Announce_type = buffer[0];
            if (Announce_type != 1) {
                if (Announce_type == 3) {
                    state = INVALID_COMMAND;
                    break;
                }
                printf("Wrong type, expected Announce\n");
                state = ERROR_STATE;
                break;
            }

            // Validate Announce
            song_size = (int)buffer[1];
            if (recv_data < song_size + 2) {
                printf("Announce message too short, closing...\n");
                state = EXIT_PROGRAM;
                break;
            } else if (recv_data > (song_size + 2) && buffer[song_size + 2] != '\0') {
                printf("Announce message too long, closing...\n");
                state = EXIT_PROGRAM;
                break;
            }

            // Extract the song name
            song_name = (char*)malloc(song_size + 1);
            song_name[song_size] = '\0';
            strcpy(song_name, buffer + 2);
            printf("Song name: %s\n", song_name);
            free(song_name);

            // Instruct the playsong thread to change station
            wait = 1;
            change_station = 1;

            // Calculate the new multicast IP (ip_addr) based on user_station
            // Only the last two octets are changed (the code below replicates that logic)
            if ((multicast_group[3] + user_station) > 255) {
                int carry = (multicast_group[3] + user_station) - 255;
                if (multicast_group[2] == 255) {
                    ip_addr[2] = 0;
                    ip_addr[1] = multicast_group[1] + 1;
                } else {
                    ip_addr[2] = multicast_group[2] + 1;
                }
                ip_addr[3] = (unsigned char)(carry - 1);
            } else {
                ip_addr[3] = (unsigned char)(multicast_group[3] + user_station);
                ip_addr[2] = multicast_group[2];
            }

            wait = 0;  // now the new IP is ready
            state = MAIN_MENU;
        } break;

        //---------------------------------------------
        // UP_SONG
        //---------------------------------------------
        case UP_SONG: {
            printf("To upload a song, enter its .mp3 filename (up to 200 chars)\n"
                   "To return to the main menu, enter 'q'\n");
            FD_ZERO(&fdset1);
            FD_SET(fileno(stdin), &fdset1);
            select_num = select(1, &fdset1, NULL, NULL, NULL);
            if (select_num < 0) {
                perror("Select -UpSong- Failed");
                state = ERROR_STATE;
                break;
            }

            if (FD_ISSET(0, &fdset1)) {
                memset(input, '\0', SIZE);
                recv_data = read(0, input, SIZE);
                if (recv_data < 0) {
                    perror("Reading -UpSong- Failed");
                    state = ERROR_STATE;
                    break;
                }
            }

            // User wants to quit uploading
            if (input[0] == 'q' && input[1] == '\n') {
                state = MAIN_MENU;
                break;
            }

            // Prepare the UpSong message
            memset(buffer, '\0', SIZE);
            buffer[0] = 2;  // UpSong type
            strcpy(up_song_name, input);
            up_song_name[strlen(input) - 1] = '\0'; // remove trailing newline

            // Check if it’s a .mp3 file
            if (strlen(up_song_name) < 4 ||
                strcmp(up_song_name + (strlen(up_song_name) - 4), ".mp3") != 0) {
                printf("Not an .mp3 file; please retry.\n");
                state = MAIN_MENU;
                break;
            }

            FILE* upload_song = fopen(up_song_name, "r");
            if (upload_song == NULL) {
                printf("File not found, please try another filename.\n");
                state = MAIN_MENU;
                break;
            }

            fseek(upload_song, 0L, SEEK_END);
            upsong_size = (uint32_t)ftell(upload_song);
            rewind(upload_song);

            // Validate size constraints
            if (upsong_size < 2000) {
                printf("Song too small (< 2000B). Upload aborted.\n");
                fclose(upload_song);
                state = MAIN_MENU;
                break;
            }
            if (upsong_size > 10485760) { // 10 MiB
                printf("Song too large (> 10MiB). Upload aborted.\n");
                fclose(upload_song);
                state = MAIN_MENU;
                break;
            }

            // Populate UpSong message
            upsong_name_size = (uint8_t)strlen(up_song_name);
            uint32_t upsong_size_net = htonl(upsong_size);

            memcpy(buffer + 1, &upsong_size_net, 4); // 4-byte file size
            buffer[5] = upsong_name_size;
            memcpy(buffer + 6, up_song_name, upsong_name_size);

            // Send the UpSong request
            int len = 6 + upsong_name_size;
            send_data = send(tcp_socket, buffer, len, 0);
            if (send_data < 0) {
                perror("Sending UpSong Failed");
                fclose(upload_song);
                state = ERROR_STATE;
                break;
            }

            // Wait for Permit message
            memset(buffer, '\0', SIZE);
            FD_ZERO(&fdset1);
            FD_SET(tcp_socket, &fdset1);
            select_num = select(FD_SETSIZE, &fdset1, NULL, NULL, &permit_time);
            permit_time.tv_usec = 300000; // reset

            if (select_num < 0) {
                perror("Select upSong Failed");
                fclose(upload_song);
                state = ERROR_STATE;
                break;
            } else if (select_num == 0) {
                printf("Timeout waiting for Permit, connection terminated.\n");
                fclose(upload_song);
                state = ERROR_STATE;
                break;
            } else if (FD_ISSET(tcp_socket, &fdset1)) {
                recv_data = recv(tcp_socket, buffer, SIZE, 0);
                if (recv_data < 0) {
                    perror("Reading Permit Failed");
                    fclose(upload_song);
                    state = ERROR_STATE;
                    break;
                }
            }

            if (buffer[0] != 2) {
                if (buffer[0] == 3) {
                    state = INVALID_COMMAND;
                    fclose(upload_song);
                    break;
                }
                printf("Expected Permit message, got something else.\n");
                fclose(upload_song);
                state = ERROR_STATE;
                break;
            }
            if (recv_data < 2) {
                printf("Permit message too short, closing...\n");
                fclose(upload_song);
                state = EXIT_PROGRAM;
                break;
            } else if (recv_data > 2 && buffer[2] != '\0') {
                printf("Permit message too long, closing...\n");
                fclose(upload_song);
                state = EXIT_PROGRAM;
                break;
            }

            // Permit = 0 => can't upload now
            if (buffer[1] == 0) {
                printf("Server can't accept the song now. Try again later.\n");
                fclose(upload_song);
                state = MAIN_MENU;
                break;
            }

            // If permitted, start sending the file
            sum_of_bytes = 0;
            p_song_sent  = 0.0;
            memset(buffer, '\0', SIZE);

            fd_set fd_read, fd_write;
            struct timeval timeout;
            timeout.tv_sec  = 0;
            timeout.tv_usec = 800; // microseconds

            int Upsong_error = 0;
            while (!feof(upload_song)) {
                FD_ZERO(&fd_read);
                FD_ZERO(&fd_write);
                FD_SET(tcp_socket, &fd_read);
                FD_SET(tcp_socket, &fd_write);

                int sel_result = select(FD_SETSIZE, &fd_read, &fd_write, NULL, &timeout);
                if (sel_result < 0) {
                    perror("Select during UpSong failed");
                    Upsong_error = 1;
                    state = ERROR_STATE;
                    break;
                } else if (sel_result == 0) {
                    // If no readiness, wait a little and retry
                    usleep(TIME_INTERVAL);
                    timeout.tv_usec = 800;
                    continue;
                } else {
                    // If the socket is ready for writing, send next chunk
                    if (FD_ISSET(tcp_socket, &fd_write)) {
                        int num_bytes = fread(buffer, 1, KIB, upload_song);
                        usleep(TIME_INTERVAL);
                        send_data = send(tcp_socket, buffer, num_bytes, 0);
                        if (send_data < 0) {
                            perror("Send song failed");
                            Upsong_error = 1;
                            state = ERROR_STATE;
                            break;
                        }
                        sum_of_bytes += send_data;
                        p_song_sent = (double)sum_of_bytes / (double)upsong_size;
                        printf("\rSending song: %dB of %dB (%.2lf%%) ",
                               sum_of_bytes, upsong_size, 100 * p_song_sent);
                        fflush(stdout);
                        memset(buffer, '\0', SIZE);
                        timeout.tv_usec = 800;
                    }
                    // If the server sent something mid-upload
                    if (FD_ISSET(tcp_socket, &fd_read)) {
                        recv_data = recv(tcp_socket, buffer, SIZE, 0);
                        if (recv_data < 0) {
                            perror("Reading while UpSong failed");
                            Upsong_error = 1;
                            state = ERROR_STATE;
                            break;
                        }
                        // If invalid command
                        if (buffer[0] == 3) {
                            Upsong_error = 1;
                            state = INVALID_COMMAND;
                            break;
                        } else {
                            printf("\nServer sent something unexpected during UpSong, terminating.\n");
                            Upsong_error = 1;
                            state = ERROR_STATE;
                            break;
                        }
                    }
                }
                if (Upsong_error) break;
            }
            fclose(upload_song);
            printf("\n");

            if (Upsong_error) {
                break;
            }

            // Finished uploading, now wait for NewStation
            FD_ZERO(&fdset1);
            FD_SET(tcp_socket, &fdset1);
            NewStation_time.tv_sec = 2;
            select_num = select(FD_SETSIZE, &fdset1, NULL, NULL, &NewStation_time);
            memset(buffer, '\0', SIZE);

            if (select_num < 0) {
                perror("Select upSong Failed after sending");
                state = ERROR_STATE;
                break;
            } else if (select_num == 0) {
                printf("Timeout waiting for NewStation, terminating...\n");
                state = ERROR_STATE;
                break;
            } else if (FD_ISSET(tcp_socket, &fdset1)) {
                recv_data = recv(tcp_socket, buffer, SIZE, 0);
                if (recv_data < 0) {
                    perror("Reading after UpSong Failed");
                    state = ERROR_STATE;
                    break;
                }
            }

            // If we got NewStations
            if (buffer[0] == 4) {
                if (recv_data < 3) {
                    printf("NewStations message too short, closing...\n");
                    state = EXIT_PROGRAM;
                    break;
                } else if (recv_data > 3 && buffer[3] != '\0') {
                    printf("NewStations message too long, closing...\n");
                    state = EXIT_PROGRAM;
                    break;
                }
                numStations = (buffer[1] << 8) + (unsigned char)buffer[2];
                printf("NewStations arrived, we have %u stations.\n\n", numStations);
            }
            // If invalid command
            else if (buffer[0] == 3) {
                state = INVALID_COMMAND;
                break;
            }
            // Unexpected message
            else {
                state = ERROR_STATE;
                break;
            }
            // Return to MAIN_MENU after successful upload
            state = MAIN_MENU;
            NewStation_time.tv_sec = 2;
        } break;

        //---------------------------------------------
        // EXIT_PROGRAM
        //---------------------------------------------
        case EXIT_PROGRAM: {
            printf("Bye Bye...\n");
            wait = 0;
            play_flag = 0;
            // Wait for the playsong thread
            pthread_join(recv_song, NULL);
            pthread_exit(&control);
        } break;

        //---------------------------------------------
        // INVALID_COMMAND
        //---------------------------------------------
        case INVALID_COMMAND: {
            printf("Invalid command\n");
            int size_invalid = (unsigned int)buffer[1];
            // Validate the size
            if (recv_data < 2 + size_invalid) {
                printf("InvalidCommand message too short, closing...\n");
                state = EXIT_PROGRAM;
                break;
            } else if (recv_data > (2 + size_invalid) && buffer[2 + size_invalid] != '\0') {
                printf("InvalidCommand message too long, closing...\n");
                state = EXIT_PROGRAM;
                break;
            }
            // Print server reason
            char* replyString = (char*)malloc(size_invalid + 1);
            replyString[size_invalid] = '\0';
            strcpy(replyString, buffer + 2);
            printf("%s\n", replyString);
            free(replyString);

            // Terminate after receiving invalid command
            play_flag = 0;
            pthread_join(recv_song, NULL);
            pthread_exit(&control);
        } break;

        //---------------------------------------------
        // ERROR_STATE
        //---------------------------------------------
        case ERROR_STATE: {
            printf("Error, terminating connection...\n");
            play_flag = 0;
            wait = 0;
            pthread_join(recv_song, NULL);
            pthread_exit(&control);
        } break;

        default:
            // Should never get here
            state = ERROR_STATE;
            break;
        }
    }

    return NULL; // Just to satisfy compiler
}

/**
 * Thread function that joins a multicast group and plays the
 * incoming audio stream by piping it to an external player.
 */
void* playsong(void *ip) {
    struct ip_mreq mreq;
    int multi_sock;
    char buffer[SIZE];
    char multi_ip[15];
    struct sockaddr_in multiaddr;
    socklen_t addr_size = sizeof(multiaddr);
    struct timeval timeout;
    fd_set fdset2;

    // Prepare external player pipe
    FILE *fp = popen("play -t mp3 -> /dev/null 2>&1", "w");
    if (fp == NULL) {
        perror("Cannot open pipe to player");
        pthread_exit(&recv_song);
    }

    // Create UDP socket
    multi_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (multi_sock < 0) {
        perror("Multicast socket creation failed");
        pclose(fp);
        pthread_exit(&recv_song);
    }

    // Bind to any address on portNumber
    multiaddr.sin_family      = AF_INET;
    multiaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    multiaddr.sin_port        = htons(portNumber);

    if (bind(multi_sock, (struct sockaddr*)&multiaddr, sizeof(multiaddr)) < 0) {
        perror("Bind -multi_sock- Failed");
        close(multi_sock);
        pclose(fp);
        pthread_exit(&recv_song);
    }

    // Build initial multicast group from ip_addr
    sprintf(multi_ip, "%d.%d.%d.%d",
            (int)ip_addr[0], (int)ip_addr[1],
            (int)ip_addr[2], (int)ip_addr[3]);

    mreq.imr_multiaddr.s_addr = inet_addr(multi_ip);
    mreq.imr_interface.s_addr = htonl(INADDR_ANY);

    // Join the group
    setsockopt(multi_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

    // Setup a short timeout in case the server stops sending
    timeout.tv_sec  = 0;
    timeout.tv_usec = 300000; // 300 ms

    while (play_flag) {
        FD_ZERO(&fdset2);
        FD_SET(multi_sock, &fdset2);

        int select_num = select(FD_SETSIZE, &fdset2, NULL, NULL, &timeout);
        if (select_num < 0) {
            perror("Select on multicast socket failed");
            break;
        } else if (select_num == 0) {
            // Timed out => likely the server is not sending
            play_flag = 0;
            continue;
        } else {
            // Data arrived on the multicast socket
            int num_of_bytes = recvfrom(multi_sock, buffer, KIB, 0,
                                        (struct sockaddr*)&multiaddr, &addr_size);
            if (num_of_bytes <= 0) {
                play_flag = 0;
                continue;
            }
            // Write to the audio player
            fwrite(buffer, 1, num_of_bytes, fp);

            // If user changed station
            if (change_station) {
                // Leave the old group
                setsockopt(multi_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));

                // Wait until IP is updated
                while (wait) { /* spin */ }

                // Join the new station group
                sprintf(multi_ip, "%d.%d.%d.%d",
                        (int)ip_addr[0], (int)ip_addr[1],
                        (int)ip_addr[2], (int)ip_addr[3]);
                mreq.imr_multiaddr.s_addr = inet_addr(multi_ip);
                mreq.imr_interface.s_addr = htonl(INADDR_ANY);
                setsockopt(multi_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

                change_station = 0;
            }
            // Reset the timeout after successful recv
            timeout.tv_usec = 300000;
        }
    }

    pclose(fp);
    close(multi_sock);
    pthread_exit(&recv_song);
}

//---------------------------------------------------
// The following are stubs left in place to preserve
// original function prototypes if they are used
// elsewhere or you want to add to them.
//---------------------------------------------------
void* UpSong(void* tcp_soc) {
    // Stub if you need a separate "UpSong" thread in the future
    return NULL;
}

void EndFunc() {
    // Clean up resources if needed
}

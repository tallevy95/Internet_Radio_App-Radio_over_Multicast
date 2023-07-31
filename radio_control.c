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


//--------- GLOBAL VARS -------------
pthread_t recv_song;		// recive songs from the server via UDP 
pthread_t control;		// control vs the server
pthread_t send_song;		// send songs to the server via TCP
unsigned char multicast_group[4];// multicast ip for station 0
uint16_t numStations,portNumber;
char* server_IP;// IP of server
int play_flag = 1;// flagfor exit from the multicast thread
int change_station = 0;// flag for change station in the multicast thread
int wait=1;// wait for ip change in the multicast thread
int flag_termniate=0;
unsigned char ip_addr[4];// multicsat address of current station


struct timeval welcome_time;// welcome timout, if dont arrive in 300 ms
struct timeval announce_time;// announce timout, if dont arrive in 300 ms
struct timeval permit_time;// permit timout, if dont arrive in 300 ms
struct timeval NewStation_time;//newstation timout, if dont arrive in 2s after finish uploading song


//--------- FUNCTIONS --------------
void* UpSong(void* tcp_soc);
void EndFunc();		// close all memory and threads
void* control_func(void *tcp_soc);
void* playsong(void *ip);


int main(int agrc, char *argv[]){
	int i, select_num;
	int tcp_port;		// TCP port
	int send_data=0,recv_data=0;
	int R_socket;		// R_socket == number of the FD_SET
	int state;		// state of the switch case
	char buffer[SIZE];
	unsigned char hello_msg[3]= {'\0'};
	fd_set fdset,tempset;
	struct sockaddr_in serverAddr;
	struct sockaddr_storage serverStorage;
	uint8_t welcome_type;
	socklen_t addr_size;
	fflush(stdout);
	///// timers definition
	welcome_time.tv_sec =0;
	welcome_time.tv_usec = 300000;
	announce_time.tv_sec =0;
	announce_time.tv_usec = 300000;
	permit_time.tv_sec =0;
	permit_time.tv_usec = 300000;
	NewStation_time.tv_sec =2;
	NewStation_time.tv_usec = 0;
	//////
	FD_ZERO(&fdset);
	memset(buffer, '\0', sizeof(buffer));
	tcp_port = (short) strtol(argv[2],NULL,10); 
	R_socket = socket(PF_INET, SOCK_STREAM, 0);		// open TCP socket
	if (R_socket<0){
		perror("Socket Failed");
		close(R_socket);
		exit(EXIT_FAILURE);
	}
	printf("Connecting to server...\n");

	/* Set properties to R_socket */
	serverAddr.sin_family = AF_INET;			// address family - IPv4
	serverAddr.sin_port = htons(tcp_port);			// which port
	serverAddr.sin_addr.s_addr = inet_addr(argv[1]);	// IP address
	memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);	// padding field to 0
	/* END OF Set properties to R_socket */
	
	/* connect the socket to server */
	addr_size = sizeof(serverAddr);

	if((connect(R_socket, (struct sockaddr*) &serverAddr, addr_size))<0){
		perror("Socket Connection Failed");
		close(R_socket);
		exit(EXIT_FAILURE);
	}

	printf("Socket Connection Succeed.\n");
	////
	struct timeval harta;
	harta.tv_sec = 0;
	harta.tv_usec = 10000;
	FD_SET(R_socket,&fdset);
	select_num = select(FD_SETSIZE,&fdset,NULL,NULL,&harta);  // if the server send message before hello
	if(select_num<0){
		perror("Select Failed");
		close(R_socket);
		exit(EXIT_FAILURE);
	}
	if (select_num>0){
		if(FD_ISSET(R_socket,&fdset)){	//welcome message arrived
			recv_data=recv(R_socket,buffer,SIZE,0);
			if(recv_data<0){
				perror("Reading Failed");
				close(R_socket);
				exit(EXIT_FAILURE);
			}
			if(recv_data>0){
				printf("message arive before hello terminate connection\n");
				close(R_socket);
				exit(1);
			}
		}
	}
	send_data = send(R_socket,hello_msg,3,0);	// sending HELLO message
	if(send_data<0){
		perror("Sending HELLO Failed");
		close(R_socket);
		exit(EXIT_FAILURE);
	}
	FD_SET(R_socket,&fdset);
	FD_SET(fileno(stdin),&fdset);

	select_num = select(FD_SETSIZE,&fdset,NULL,NULL,&welcome_time);  // waiting to WELCOME message
	if(select_num<0){
		perror("Select Failed");
		close(R_socket);
		exit(EXIT_FAILURE);
	}
	else if (select_num == 0){
		printf("Welcome timeout, closing connection...\n");
		close(R_socket);
		exit(EXIT_FAILURE);
	}
	else if(FD_ISSET(R_socket,&fdset)){	//welcome message arrived
		recv_data=recv(R_socket,buffer,SIZE,0);
		if(recv_data<0){
			perror("Reading Failed");
			close(R_socket);
			exit(EXIT_FAILURE);
		}

		welcome_type = buffer[0];
		if(recv_data < 9){
			printf("Welcome message to short,closing connection...\n");
			close(R_socket);
			return 0;
		}
		else if(recv_data>9 && buffer[9] != '\0'){
			printf("Welcome message to long,closing connection...\n");
			close(R_socket);
			return 0;
		}	
		if(welcome_type == 0){
			numStations = buffer[1];
			numStations = numStations<<8;
			numStations+= buffer[2];
			multicast_group[3] = buffer[3];
			ip_addr[3] = buffer[3];
			multicast_group[2] = buffer[4];
			ip_addr[2] = buffer[4];
			multicast_group[1] = buffer[5];
			ip_addr[1] = buffer[5];
			multicast_group[0] = buffer[6];
			ip_addr[0] = buffer[6];
			portNumber = buffer[7];
			portNumber = portNumber<<8;
			portNumber += (unsigned char)buffer[8];// for port num higher
			printf("Welcome to Radio Server %s\nMulticast address :  %d.%d.%d.%d\nThe number of stations: %d\nPort Number: %u\n",argv[1],ip_addr[0],ip_addr[1],ip_addr[2],ip_addr[3],numStations,portNumber);
			pthread_create(&control,NULL,&control_func,(void*)&R_socket);// thread for the communication with the server and user
			pthread_create(&recv_song,NULL,&playsong,(void*)ip_addr);// multicast thread for music
		}
		else if (welcome_type == 3){ // if invalid command arrived
			int size_invalid;
			char* replyString;
			size_invalid = (unsigned int)buffer[1];
			replyString=(char*)malloc(size_invalid+1);
			replyString[size_invalid]='\0';
			strcpy(replyString,buffer+2);
			printf("%s\n",replyString);
			free(replyString);
			close(R_socket);
			return 0;
		}
		else{
			printf("Error in message from server,closing connection...\n");
			close(R_socket);
			return 0;
		}
	}
	pthread_join(control,NULL);// wait for control to exit succsesfully
	close(R_socket);
	return 0;
}


void* control_func(void *tcp_soc){
	int tcp_socket,i;
	int state = 0,user_station,send_data=0;
	int select_num=0,recv_data=0;
	int sum_of_bytes=0;// sum of bytes to read in upsong
	double p_song_sent; // how much from the song we sent
	uint8_t Ask_song[3] = {1,0,0};
	uint8_t Announce_type;
	uint16_t temp_station;
	char buffer[SIZE],input[SIZE];
	int song_size;
	char *song_name;
	char up_song_name[255] = {'\0'};// song name for upload
	uint8_t upsong_name_size;// song name len for upload
	uint32_t upsong_size;// song size for upload
	char permit[2] = {'\0'};
	char send_data_up_song[3];
	char* mp3 = ".mp3";
	char mp3_check[5] ={'\0'};
	fd_set fdset1;
	FILE* upload_song;
	struct stat st;//struct for file size in Upsong
	tcp_socket = (int)(*(int *)tcp_soc);
	memset(buffer, '\0', sizeof(buffer));
	memset(input, '\0', sizeof(input));

//***
	unsigned char hello_msg[3]= {'\0'};
	int check_state  = 0;
	
//***
	while(1){	
		switch(state){
			case 0:	// main menu
				memset(input,'\0',sizeof(input));
				memset(buffer,'\0',sizeof(input));
				FD_SET(fileno(stdin),&fdset1);
				FD_SET(tcp_socket,&fdset1);
				printf("Enter:\n\t1- to AskSong\n\t2- to UpSong\n\t3- to Exit\n");
				select_num = select(FD_SETSIZE,&fdset1,NULL,NULL,NULL);
				if(select_num<0){
					perror("Select Failed");
					state = 5;
					break;
				}
				else if(FD_ISSET(0,&fdset1)){	// input feom keyboard arrived
					recv_data=read(0,input,SIZE);
					if(recv_data<0){
						perror("Reading Failed");
						state = 5;
						break;
					}			
					state = input[0] - '0';
					if(state!=1 && state!=2 && state!=3) {//wrong input
						printf("wrong input,please enter again\n");
						state = 0;
					}
				}
				else if(FD_ISSET(tcp_socket,&fdset1)){ // message from server arrived
					recv_data = read(tcp_socket,buffer,SIZE);
					if(recv_data<0){
						perror("Reading Failed");
						state = 5;
						break;
					}
					if(recv_data == 0){
						printf("Server termniate connection\n");
						state = 3;
						flag_termniate = 1;
						break;
					}
					if (buffer[0] == 4){		//new stations arrive
						if(recv_data < 3){
							printf("NewStations message to short,closing connection...\n");
							state = 3;
							break;
						}
						else if(recv_data > 3 && buffer[3] != '\0'){
							printf("NewStations message to long,closing connection...\n");
							state = 3;
							break;
						}	
						numStations = buffer[1];
						numStations = numStations<<8;
						numStations+= buffer[2];
						printf("NewStations arrive, we have %u stations.\n",numStations);
						break;
					}
					else if(buffer[0] == 3){	//invalid command arrive
						state = 4;
						break;
					}
					else {				// wrong message sent
						state = 5;
						break;
					}
				}
				break;
			case 1: // AskSong
				memset(input,'\0',sizeof(input));
				printf("Enter station number : from 0 to %u\n",numStations-1);
				FD_SET(fileno(stdin),&fdset1);// put stdin for the FD set
				select_num = select(1,&fdset1,NULL,NULL,NULL);//waiting for input from user
				if(select_num<0){
					perror("Select AskSong Failed 1");
					state = 5;
					break;
				}
				else if(FD_ISSET(0,&fdset1)){	//name of station from user
					recv_data=read(0,input,SIZE);
					if(recv_data<0){
						perror("Reading AskSong Failed");
						state = 5;
						break;
					}
				}
				user_station = input[0] - '0';// cast from value in char to value in int
				if(user_station < 0 || user_station > (numStations-1)){//wrong input
					printf("wrong input,please enter again\n");
					state = 1;
					break;
				}
				temp_station = (uint16_t)user_station;
				Ask_song[1] =  temp_station>>8;// FF00 and numStations
				Ask_song[2] = 255 & temp_station;// 00FF and numStations
				send_data = send(tcp_socket,Ask_song,3,0);	// sending Asksong message
				if(send_data<0){
					perror("Sending AskSong Failed");
					state = 5;
					break;
				}
				FD_SET(tcp_socket,&fdset1);
				select_num = select(FD_SETSIZE,&fdset1,NULL,NULL,&announce_time);// waiting for announce
				announce_time.tv_usec = 300000;
				memset(buffer,'\0',sizeof(input));
				if(select_num<0){
					perror("Select AskSong Failed 2");
					state = 5;
					break;
				}
				else if (select_num == 0){
					printf("Timeout in announce message, terminate connection...\n");
					state = 5;
					break;
				}
				else if(FD_ISSET(tcp_socket,&fdset1)){	//recive announce from server
					recv_data = recv(tcp_socket,buffer,SIZE,0);
					if(recv_data<0){
						perror("Reading Failed");
						state = 5;
						break;
					}
				}	
				Announce_type = buffer[0];
				if (Announce_type != 1){
					if (Announce_type == 3){
						state = 4;
						break;
					}
					printf("Wrong type , Announce\n");
					state = 5;
					break;
				}
				wait = 1;// wait for ip change in the multicast thread
				change_station = 1;//tells the multicast thread to change station
				song_size = (int)buffer[1];
				if(recv_data < song_size+2){
					printf("Announce message to short,closing connection...\n");
					state = 3;
					break;
				}	
				else if(recv_data > (song_size+2) && buffer[song_size+2] != '\0'){
					printf("Announce message to long,closing connection...\n");
					state = 3;
					break;
				}
				song_name = (char*)malloc(song_size+1);
				song_name[song_size]='\0';
				strcpy(song_name,buffer+2);
				printf("song name : %s \n",song_name);
				free(song_name);
				if ((multicast_group[3] + input[0]- 48)>255){// if the multicsat address is more than 256 add one to the next octet and carry to the current octet
					int carry;
					carry = (multicast_group[3] + input[0] - 48) -255;
					if(multicast_group[2] == 255){
						ip_addr[2] = 0;
						ip_addr[1] = multicast_group[1] + 1;
					}
					else { 
						ip_addr[2] = multicast_group[2] + 1;
					}
					ip_addr[3] = (char)(carry - 1);
				}				
				else {
					ip_addr[3] = multicast_group[3] + input[0]- 48;
					ip_addr[2] = multicast_group[2];
				}
				wait=0;
				state = 0;
				break;
			case 2://upsong
				printf("If you want to upload a song - please enter the name of the song you want to\nupload (for example: 'song.mp3')\n200 charecters TOPS\n");
				printf("To get back to the main menu enter 'q'\n");
				memset(input,'\0',sizeof(input));
				FD_SET(fileno(stdin),&fdset1);
				select_num = select(1,&fdset1,NULL,NULL,NULL);
				if(select_num<0){
					perror("Select -Upsong- Failed");
					state = 5;
					break;
				}
				else if(FD_ISSET(0,&fdset1)){	//an stdin interupt
					recv_data=read(0,input,SIZE);
					if(recv_data<0){
						perror("Reading -Upsong- Failed");
						state = 5;
						break;
					}
				}
				if(input[0] == 'q' && input[2] == '\0'){// if the user want to quit
					state = 0;
					break;
				}
				memset(buffer,'\0',sizeof(buffer));
				buffer[0] = 2;//set the Upsong message
				strcpy(up_song_name,input);
				up_song_name[strlen(input)-1] = '\0';
				int a =strlen(up_song_name)-4;
				strcpy(mp3_check,up_song_name + a);// checks if the name of the file from the user input is .mp3
				if(strcmp(mp3,mp3_check)){
					printf("not a mp3 file,please enter a valid mp3 file\n");
					state = 0;
					break;
				}
				upload_song = fopen(up_song_name,"r"); //open file
				if (upload_song == NULL){//check if the file exist in the computer
					printf("No such song,please enter an exist song name.\n");
					state = 0;
					break;
				}
				fseek(upload_song,0L,SEEK_END);
				upsong_size = (uint32_t)htonl(ftell(upload_song));// file size
				rewind(upload_song);	
				upsong_name_size = (uint8_t)strlen(up_song_name);			
				memcpy(buffer+1,&upsong_size,4);
				buffer[5] = upsong_name_size;
				memcpy(buffer+6,up_song_name,upsong_name_size);
				upsong_size = ntohl(upsong_size);
				if (upsong_size<2000){//if the song size is less then 2000B go back to connection establish 
					printf("The song size is to small.\nplease try to send a song withe size between 2000B and 10MiB.\n");
					state = 0;
					fclose(upload_song);
					break;
				}
				if (upsong_size>10485760){//if the song size is less then 10MiB go back to connection establish 
					printf("The song size is to big.\nplease try to send a song withe size between 2000B and 10MiB.\n");
					state = 0;
					fclose(upload_song);
					break;
				}
				int len = 6 + upsong_name_size;// the length of the message
				send_data = send(tcp_socket,buffer,len,0);	// sending UpSong message
				if(send_data<0){
					perror("Sending UpSong Failed");
					state = 5;
					break;
				}
				memset(buffer,'\0',SIZE);
				FD_SET(tcp_socket,&fdset1);
				select_num = select(FD_SETSIZE,&fdset1,NULL,NULL,&permit_time);// waiting for Permit
				permit_time.tv_usec = 300000;
				
				if(select_num<0){
					perror("Select upSong Failed ");
					state = 5;
					break;
				}
				else if (select_num == 0){// if timeout occure terminate
					printf("Timeout in permit message, terminate connection...\n");
					state = 5;
					break;
				}
				else if(FD_ISSET(tcp_socket,&fdset1)){	//recive announce from server
					recv_data=recv(tcp_socket,buffer,SIZE,0);
					if(recv_data<0){
						perror("Reading Failed");
						state = 5;
						break;
					}
				}
				if(buffer[0]!=2){
					if(buffer[0] == 3){
						state = 4;//invalid command
						break;
					}
					printf("Wrong type , expect to get Permit\n");
					state = 5;
					break;
				}
				if(recv_data < 2){
					printf("Permit message to short,closing connection...\n");
					state = 3;
					break;
				}	
				else if(recv_data > 2 && buffer[2] != '\0'){
					printf("Permit message to long,closing connection...\n");
					state = 3;
					break;
				}
				
				if(buffer[1] == 0){// if permit is 0 return to connection establish 
					printf("Can't send song right now,please try again later.\n");
					state = 0;
					fclose(upload_song);
					break;
				}
				int num_bytes,Upsong_error=0;
				uint32_t temp_upsong = upsong_size;				
				fd_set fwrite;
				memset(buffer,'\0',SIZE);
				struct timeval timeout;//timeout of select of send UpSong
				timeout.tv_sec=0;
				timeout.tv_usec=800;
				FD_SET(tcp_socket,&fdset1);
				FD_SET(tcp_socket, &fwrite);
				while(!(feof(upload_song))){
					select_num = select(FD_SETSIZE,&fdset1,&fwrite,NULL,&timeout);
					if(select_num<0){
						perror("Send song failed\n");
						state=5;
						fclose(upload_song);
						break;
					}
					else if(select_num==0){//if timeout wait for TIME_INTERVAL usec and then try again
						usleep(TIME_INTERVAL);
						timeout.tv_usec=800;
						continue;	
					}
					else if(FD_ISSET(tcp_socket,&fwrite)){	
						num_bytes = fread(buffer,1,KIB,upload_song);
						usleep(TIME_INTERVAL);						
						send_data = send(tcp_socket,buffer,num_bytes,0);// sending the song
						if(send_data<0){
							perror("Send song failed\n");
							state=5;
							fclose(upload_song);	
							break;
						}
						sum_of_bytes += send_data;
						p_song_sent = (double)(((double)sum_of_bytes)/((double)upsong_size));
						printf("\rSending song, we sent %dB from %dB of the file (%.2lf%%)",sum_of_bytes,upsong_size,100*p_song_sent);//
						fflush(stdout);
						memset(buffer,'\0',SIZE);
						FD_SET(tcp_socket, &fwrite);
						timeout.tv_usec=800;
					}
					else if(FD_ISSET(tcp_socket,&fdset1)){
						recv_data=recv(tcp_socket,buffer,SIZE,0);
						if(recv_data<0){
							perror("Reading Failed");
							state = 5;
							break;
						}
						if(buffer[0] == 3){
							state= 4;
							Upsong_error = 1;
							break;
						}
						else {
							printf("The server sent something while UpSong, terminate connection\n");
							state =5;
							Upsong_error = 1;
							break; 
						}
					}
				}
				fclose(upload_song);	
				printf("\n");
				if(Upsong_error){
					break;
				}
				sum_of_bytes=0;
				p_song_sent=0.0;
				FD_SET(tcp_socket,&fdset1);
				NewStation_time.tv_sec =2;
				select_num = select(FD_SETSIZE,&fdset1,NULL,NULL,&NewStation_time);// waiting for newstation message
				memset(buffer,'\0',sizeof(buffer));
				if(select_num<0){
					perror("Select upSong Failed ");
					state = 5;
					break;
				}
				else if (select_num == 0){
					printf("Timeout in NewStation message after UpSong, terminate connection...\n");
					state = 5;
					break;
				}
				else if(FD_ISSET(tcp_socket,&fdset1)){	//recive announce from server
					recv_data=recv(tcp_socket,buffer,SIZE,0);
					if(recv_data<0){
						perror("Reading Failed");
						state = 5;
						break;
					}
				}				
				if (buffer[0] == 4){//new stations arrive
						if(recv_data < 3){
							printf("NewStations message to short,closing connection...\n");
							state = 3;
							break;
						}
						else if(recv_data > 3 && buffer[3] != '\0'){
							printf("NewStations message to long,closing connection...\n");
							state = 3;
							break;
						}
						numStations = buffer[1];
						numStations = numStations<<8;
						numStations+= buffer[2];
						printf("NewStations arrive, we have %u stations.\n\n",numStations);
				}
				else if(buffer[0] == 3){//invalid command arrive
					state = 4;
					break;
				}
				else {// wrong message sent
					state = 5;
					break;
				}	
				state = 0;
				NewStation_time.tv_sec =2;
				break;
			case 3://exit
				printf("Bye Bye...\n");
				wait = 0;
				play_flag = 0;
				pthread_join(recv_song,NULL);
				pthread_exit(&control);
			case 4://invalid command
				printf("Invalid command\n");
				int size_invalid;
				char* replyString;
				size_invalid = (unsigned int)buffer[1];
				if(recv_data < 2 + size_invalid){
						printf("InvalidCommand message to short,closing connection...\n");
						state = 3;
						break;
					}
					else if(recv_data > (2+size_invalid) && buffer[2+size_invalid] != '\0'){
						printf("InvalidCommand message to long,closing connection...\n");
						state = 3;
						break;
					}
				replyString=(char*)malloc(size_invalid+1);
				replyString[size_invalid]='\0';
				strcpy(replyString,buffer+2);
				printf("%s\n",replyString);
				free(replyString);
				play_flag = 0;
				pthread_join(recv_song,NULL);
				pthread_exit(&control);
			case 5://ERROR 					
				printf("Error,terminate connection...\n");				
				play_flag = 0;
				wait = 0;
				pthread_join(recv_song,NULL);
				pthread_exit(&control);
				break;
		}
	}
}


void* playsong(void *ip){
	struct ip_mreq mreq;
	int multi_sock,i;
	unsigned char multi_ip[15];
	int select_num;
	char buffer[SIZE];
	FILE * fp;
	struct sockaddr_in multiaddr;
	socklen_t addr_size;
	struct timeval timeout;
	fd_set fdset2;
	FD_ZERO(&fdset2);
	memset(buffer, '\0', sizeof(buffer));
	sprintf(multi_ip,"%d.%d.%d.%d",(int)ip_addr[0],(int)ip_addr[1],(int)ip_addr[2],(int)ip_addr[3]);
	fp = popen("play -t mp3 -> /dev/null 2>&1","w");// open file to play
	if (fp == NULL){
		perror("Cant open file");
		close(multi_sock);
		exit(EXIT_FAILURE);
	}
	multi_sock = socket(AF_INET, SOCK_DGRAM, 0);		// open multicast socket
	if (multi_sock<0){
		perror("Socket -multi_sock- Failed");
		close(multi_sock);
		exit(EXIT_FAILURE);
	}
	multiaddr.sin_family=AF_INET;
	multiaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	multiaddr.sin_port = htons(portNumber);
	
	if(bind(multi_sock,(struct sockaddr*)&multiaddr,sizeof(multiaddr))<0){
		perror("Bind -multi_sock- Failed");
		close(multi_sock);
		exit(EXIT_FAILURE);
	}
	mreq.imr_multiaddr.s_addr = inet_addr(multi_ip);
	mreq.imr_interface.s_addr = htonl(INADDR_ANY);
	addr_size = sizeof multiaddr;
	setsockopt(multi_sock,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreq,sizeof(mreq));// join the multicast group
	timeout.tv_sec = 0;
	timeout.tv_usec = 300000;// timeout if server terminate connection
	while(play_flag){
		FD_SET(multi_sock,&fdset2);
		select_num = select(FD_SETSIZE,&fdset2,NULL,NULL,&timeout);
		if(select_num<0){
			perror("Select multi failed\n");
			break;
		}
		else if(select_num==0){//if timeout, server terminate connection
			play_flag = 0;
			continue;	
		}
		else{		
			int num_of_bytes = recvfrom(multi_sock,buffer,KIB,0,(struct sockaddr*)&multiaddr,&addr_size);
			fwrite(buffer,1,num_of_bytes,fp);
			if(change_station){
				setsockopt(multi_sock,IPPROTO_IP,IP_DROP_MEMBERSHIP,&mreq,sizeof(mreq));// leave the multicast group	
				while(wait){}		
				sprintf(multi_ip,"%d.%d.%d.%d",(int)ip_addr[0],(int)ip_addr[1],(int)ip_addr[2],(int)ip_addr[3]);
				mreq.imr_multiaddr.s_addr = inet_addr(multi_ip);
				mreq.imr_interface.s_addr = htonl(INADDR_ANY);
				addr_size = sizeof multiaddr;
				setsockopt(multi_sock,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreq,sizeof(mreq));// join the multicast group
				change_station=0;
			}
			timeout.tv_usec = 300000;
		}
	}
	pclose(fp);
	close(multi_sock);
	pthread_exit(&recv_song);
}







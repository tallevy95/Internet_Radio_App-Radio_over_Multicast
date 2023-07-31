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
#define	MAX_CLIENTS 100
#define KIB 1024
#define TIME_INT 62500

//--------- TYPE OF SERVER MESSAGES -------
struct Welcome{
	uint8_t	 replyType;	// 0
	uint16_t numStations;
	uint8_t	 multicastGroup[4];	// Multicast Group IP
	uint16_t portNumber;		// UDP port
};

struct Announce{
	uint8_t	 replyType;	// 1 
	uint8_t	 songNameSize;
	char* 	 songName;
};

struct PermitSong{
	uint8_t  replyType;	// 2
	uint8_t  permit;
};

struct InvalidCommand{
	uint8_t	 replyType;	// 3
	uint8_t	 replyStringSize;
	char*	 replyString;
};

struct NewStations{
	uint8_t  replyType;	//4
	uint16_t newStationNumber;
};

struct Station{
	uint8_t	 multicastGroup[4];	// Multicast Group IP
	uint8_t	 songNameSize;
	char* 	 songName;
};

struct UpSong{
	uint32_t songSize;
	uint8_t songNameSize;
	char* 	 songName;
};

//--------- GLOBAL VARS -------------
pthread_t control;				// control the main manu
pthread_t client_list[MAX_CLIENTS] = {'\0'};	// list of all the clients - use to control and to send broadcast msg
pthread_t *station_list;			// list of all stations threads - size need to keep updating
pthread_mutex_t mutex_up;
struct Station* station_names;			// list of all stations names+IP - size need to keep updating
struct Welcome welcome;				// need to be global for the multicastGroup IP and for the portNumber | also will be updated numStations for new clients

int flags[3] = {0,0,1};				// [flag for full clients, flag for creating threads in playsong, flag for inf while in playsong
int num_clients=0;
int client_socket[MAX_CLIENTS] = {0};
int global_running=1;//flag to stop all client threads
int someone_sending=0;// if some client send a song


//--------- FUNCTIONS --------------
int OpenWelcomeSocket(uint16_t port);
void* manage_clients_control( void* tcp_socket);	// switch-case
void* play_song(void* stationNum);	// function that every station play in a loop. | will need to get the number of station to get the right IP for it
void IP_Check(int station_index);	// check & save new station ip		//************


int main(int argc, char *argv[]){	// <tcpPort> <mulicastIP> <udpPort> <file1> <file2> ......
	int num_files;
	uint16_t tcp_port, udp_port;
	int Welcome_socket;
	int data_recv,data_send;
	int num_socket=0;
	int i, j;
	char* substring = ".mp3";
	char* ptr;	
	struct sockaddr_in welcomeAddr;
	struct sockaddr_storage serverStorage;
	socklen_t client_size,welcome_size, addr_size;
	fd_set fdset, tempset;
	
	if(pthread_mutex_init(&mutex_up,NULL)!=0){
		perror("Mutex init has faild\n");
		exit(1);
	}
	memset(welcomeAddr.sin_zero,'\0',sizeof(welcomeAddr.sin_zero));

	if(argc < 5){
		printf("Invalid Input !\nThe input must be in this format, with the first 4 are a MUST:\n<tcpPort> <mulicastIP> <udpPort> <file1.mp3> <file2.mp3> ......\n");
		exit(1);
	}
	
	tcp_port =  (uint16_t) strtol(argv[1],NULL,10);				// will be sent to the function
	welcome.portNumber = (uint16_t) strtol(argv[3],NULL,10);		// saving the UDP pornt to the global var
	inet_pton(AF_INET, argv[2], welcome.multicastGroup);			// saving the multicast IP to the global var
	welcome.numStations = (uint16_t) argc - (uint16_t) 4;			// get the number of the files in the start
	welcome.replyType = 0;							// type of the message for the client to know
	num_clients = 0;							// number of clients at the start
	if(welcome.numStations > 0){
		num_files = argc - 4;
		station_list = (pthread_t*)malloc(sizeof(pthread_t)*num_files);	// list of stations
		station_names = (struct Station*)malloc(sizeof(struct Station)*num_files);
		if(station_names == NULL || station_list == NULL){
			free(station_list);
			free(station_names);
			perror("malloc didn't worked");
			exit(1);
		}
		for(i=0; i<num_files; i++){
			int num = i;
			inet_pton(AF_INET, argv[2], station_names[i].multicastGroup);
			IP_Check(i);	// check & fix ip -> x.1.2.3	//************
			station_names[i].songNameSize = sizeof(argv[i+4]);
			station_names[i].songName = (char*)malloc(sizeof(char)*sizeof(argv[i+4]));
			strcpy(station_names[i].songName,argv[i+4]);
			flags[1] = 0;
			pthread_create(&station_list[i], NULL, &play_song, (void*) &num);	// open thread to the station
			while(!flags[1]){}// wait for thread to open a multicast station
		}
	}

	Welcome_socket = OpenWelcomeSocket(tcp_port);// open welcome socket
	if(Welcome_socket<0) {
		close(Welcome_socket);
		exit(EXIT_FAILURE);
	}
	
	FD_ZERO(&fdset);
	FD_SET(Welcome_socket, &fdset);
        FD_SET(fileno(stdin),&fdset);

	int state = 0;
	int select_num, recv_data=0;
	char input[SIZE];
	memset(input, '\0', sizeof(input));
	printf("Welcome to the Radio Server !\n");
	printf("Choose your next move:\n\t1 - To see all the current Stations & Clients\n\t2 - To Exit\n");
	int end_run=1;//flag to end the server	
	while(end_run){
		switch(state){
			case 0:	// print menu
				memset(input,'\0',sizeof(input));
				FD_SET(Welcome_socket, &fdset);
				FD_SET(fileno(stdin),&fdset);
				select_num = select(FD_SETSIZE,&fdset,NULL,NULL,NULL);
				if(select_num<0){
					perror("Select - case 0 | main - Failed");
					state = 2;
					break;
				}
				else if(FD_ISSET(0,&fdset)){// stdin input 
					recv_data = read(0,input,SIZE);
					if(recv_data<0){
						perror("Reading - case 0 | main - Failed");
						state = 2;
						break;
					}			
					state = input[0] - '0';
					if(state!=1 && state!=2) {//wrong input
						printf("Wrong input,please enter again\n");
						state = 0;
						printf("Choose your next move:\n\t1 - To see all the current Stations & Clients\n\t2 - To Exit\n");
						break;
					}
				}
				else if(FD_ISSET(Welcome_socket,&fdset)){// welcome socket jump
					int index=0;
					while(index<MAX_CLIENTS && client_socket[index]!=0) index++;//find next open spot
					if( index == MAX_CLIENTS ){
						printf("There is no more room for clients, try again later\n");
						state = 0;
						break;
					}
					client_size = sizeof(serverStorage);
					memset(&client_socket[index],0,sizeof(client_socket[index]));
					client_socket[index] = accept(Welcome_socket,(struct sockaddr*) &serverStorage, &client_size);// accept the new socket 
					if(client_socket[index]<0){
						perror("Accept - Main - Failed");
						state = 2;
						break;
					}
					printf("***New client join the server!***\n");
					pthread_create(&client_list[index],NULL,&manage_clients_control,(void*)&index);// open thread to the new socket
					num_clients++;// number of clients
					FD_SET(Welcome_socket, &fdset);
				}
				break;
			case 1:	// print info
				if(welcome.numStations == 0){
					printf("We don't have any stations yet, try again later !\n");
					state = 0;
				}
				else{
					printf("We have %u stations!\n", welcome.numStations);
					for(i=0; i<welcome.numStations;i++){
						printf("Station %d : %d.%d.%d.%d\n\t", i, station_names[i].multicastGroup[0] ,station_names[i].multicastGroup[1] ,station_names[i].multicastGroup[2] 
										   ,station_names[i].multicastGroup[3]);
						printf("Song name : %s\n", station_names[i].songName);
					}
				}
				if(num_clients == 0){
					printf("We don't have any clients yet, try again later !\n");
					state = 0;
				}
				else{
					printf("We have %u clients!\n", num_clients);
					int temp_clients = 1;
					for(i=0; i<MAX_CLIENTS; i++){
						if(client_socket[i] != 0){
							client_size = sizeof(serverStorage);
							getpeername(client_socket[i], (struct sockaddr*) &serverStorage, &client_size); // get the ip of the peer
    							char ip_address[INET_ADDRSTRLEN]; // INET_ADDRSTRLEN = 16 the max len of a ipv4 address
							inet_ntop(AF_INET, &((struct sockaddr_in*) &serverStorage)->sin_addr,ip_address, INET_ADDRSTRLEN);// enter the ip of the peer to ip_address
							printf("Client %d with IP %s\n", temp_clients, ip_address);
							temp_clients++;
						}
					}
				}
				printf("Choose your next move:\n\t1 - To see all the current Stations & Clients\n\t2 - To Exit\n");
				state = 0;
				break;

			case 2:	// exit
				flags[2] = 0;
				for(i=0; i<welcome.numStations; i++){
					free(station_names[i].songName);
					pthread_join(station_list[i],NULL);
				}
				global_running = 0;
				for(i=0; i<MAX_CLIENTS; i++){
					if(client_list[i] != 0){
						pthread_join(client_list[i],NULL);
					}
				}
				free(station_list);
				free(station_names);
				close(Welcome_socket);
				printf("Bye Bye\n");
				end_run = 0;
				break;
		}
	}
	if(pthread_mutex_destroy(&mutex_up) != 0){
		perror("MUTEX DESTROY FAILED");
		exit(-1);	
	}
	return 1;
}


void IP_Check(int station_index){	//************
	
	int i = station_index;
	if ((station_names[i].multicastGroup[3] + i )>255){ // if the multicsat address is more than 255 add one to the next octet and carry to the current octet
		int carry;
		carry = (station_names[i].multicastGroup[3] + i ) - 255;
		if(station_names[i].multicastGroup[2] == 255){	// if the IP is in the format X.X.255.X
			station_names[i].multicastGroup[2] = 0;
			station_names[i].multicastGroup[1] = welcome.multicastGroup[1]+1;
		}
		else {
			station_names[i].multicastGroup[2] = welcome.multicastGroup[2] + 1;
		}
		station_names[i].multicastGroup[3] = (carry - 1);
	}				
	else {
		station_names[i].multicastGroup[3] = welcome.multicastGroup[3] + i ;
		station_names[i].multicastGroup[2] = welcome.multicastGroup[2];
	}
}


void* play_song(void* stationNum){
	int s, sockfd;
	struct sockaddr_in mcast_addr;
	struct ip_mreq mreq;
	char buffer[SIZE];
	unsigned char multi_ip[15];
	int ttl = 100;
	FILE *fp;
	
	s = (int)(*(int*)stationNum);//station number
	
	memset(buffer,'\0',SIZE);
	sockfd = socket(AF_INET, SOCK_DGRAM, 0);
	if(sockfd < 0){
		perror("socket - play_song - failed");
		exit(1);
	}
	sprintf(multi_ip,"%d.%d.%d.%d",station_names[s].multicastGroup[0],station_names[s].multicastGroup[1] ,station_names[s].multicastGroup[2] ,station_names[s].multicastGroup[3]);
	mcast_addr.sin_family = AF_INET;
	mcast_addr.sin_addr.s_addr = inet_addr(multi_ip);
	mcast_addr.sin_port = htons(welcome.portNumber);
	memset(mcast_addr.sin_zero, '\0', sizeof mcast_addr.sin_zero);			// padding field to 0 
	if (setsockopt(sockfd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0) {	// set ttl
		perror("setsockopt");
		exit(1);
	}
	fp = fopen(station_names[s].songName, "r");// open file for read
	if( fp == NULL ){
		perror("fopen - play_song - failed");
		exit(1);
	}
	flags[1] = 1;
	while(flags[2]){
		while(!feof(fp) && flags[2]){
			int num_byte = fread(buffer,1,KIB,fp);// read from file
			sendto(sockfd, buffer, num_byte, 0, (struct sockaddr *) &mcast_addr, sizeof(mcast_addr));//send buffer to multicast socket
			usleep(TIME_INT);
		}
		rewind(fp);// start over the file
	}
	close(sockfd); //closing multicast socket
	fclose(fp);
}


void* manage_clients_control(void* index_socket){
	int state = 0, select_num, recv_data,i,send_data,sock;
	fd_set fdset1;
	int index = (int)(*(int *)index_socket);// index for the socket and the thread 
	unsigned char buffer[SIZE];
	uint16_t station_request;		// for announce message
	uint32_t songsize_upsong;    		// size of song for upsong
	struct InvalidCommand Invalid;		// invalid command struct 
	struct Announce announce;		// announce command struct 
	struct UpSong up_song;			// upsong command struct 
	uint8_t permit0[2]={2,0};		
	uint8_t permit1[2]={2,1};
	struct NewStations new_stations;	// newstations command struct 
	char* mp3 = ".mp3";			// check if the song from upsong is mp3
	char mp3_check[5] ={'\0'};		// ^^^^
	int permit_flag= 1; 			// 0 send permit0 | 1 send permit1
	FILE* new_UpSong;
	struct timeval Hello_timeout;			// hello timeout
	struct timeval upsong_timeout;			// upsong timeout
	Hello_timeout.tv_sec = 0;
	Hello_timeout.tv_usec = 300000;			// 300 ms
	upsong_timeout.tv_sec = 3;
	upsong_timeout.tv_usec = 0;
	memset(buffer,'\0',SIZE);
	sock = client_socket[index];
	FD_SET(sock,&fdset1);
	select_num = select(FD_SETSIZE,&fdset1,NULL,NULL,&Hello_timeout);  // waiting to Hello message
	if(select_num<0){
		perror("Select - manage_clients_control - Failed");
		close(sock);
		exit(EXIT_FAILURE);
	}
	else if (select_num == 0){
		printf("Hello timeout, closing connection...\n");
		close(sock);
		exit(EXIT_FAILURE);
	}
	
	else if(FD_ISSET(sock,&fdset1)){	//Hello message arrived
		recv_data=recv(sock,buffer,SIZE,0);
		if(recv_data<0){
			perror("Reading - manage_clients_control - Failed");
			close(sock);
			exit(EXIT_FAILURE);
		}
		if(buffer[0] != 0){
			state = 5;	// wrong message -> go to case 5
		}
		if(recv_data < 3){
			state = 3;	// Invalid command -> go to case 3
			Invalid.replyType = 3;
			char message[41] = "Hello Message to short, Close connection";
			Invalid.replyStringSize = 41;
			Invalid.replyString = (char*)malloc(41*1);
			strcpy(Invalid.replyString,message);
			printf("%s\n",Invalid.replyString);
		}
		if(recv_data > 3){
			state = 3;	// Invalid command -> go to case 3
			Invalid.replyType = 3;
			char message[41] = "Hello Message to large, Close connection";
			Invalid.replyStringSize = 41;
			Invalid.replyString = (char*)malloc(41*1);
			strcpy(Invalid.replyString,message);
			printf("%s\n",Invalid.replyString);
		}
		if(buffer[1] != 0 || buffer[2] != 0){
			state = 3;	// Invalid command -> go to case 3
			Invalid.replyType = 3;
			char message[38] = "Wrong Hello Message, Close connection";
			Invalid.replyStringSize = 38;
			Invalid.replyString = (char*)malloc(38*1);
			strcpy(Invalid.replyString,message);
			printf("%s\n",Invalid.replyString);
		}
		if(state == 0){		// Hello message is ok -> send Welcome
			memset(buffer,'\0',SIZE);
			buffer[0] = welcome.replyType;
			uint16_t numstation,portnumber;
			numstation = htons(welcome.numStations);
			memcpy(buffer+1,&numstation,2);
			buffer[3] = welcome.multicastGroup[3];
			buffer[4] = welcome.multicastGroup[2];
			buffer[5] = welcome.multicastGroup[1];
			buffer[6] = welcome.multicastGroup[0];
			portnumber = htons(welcome.portNumber);
			memcpy(buffer+7,&portnumber,2);
			send_data = send(sock,buffer,9,0);	// sending UpSong message
			if(send_data<0){
				perror("Sending welcome Failed");
				state = 6;	// failed to send message -> go to case 6
			}
		}
	}
	int local_running = 1;		// flag for local running thread
	struct timeval timeout;		// timout for select -> for close server
	timeout.tv_sec = 0;
	timeout.tv_usec = 50000;	// 50ms
	while(local_running && global_running){
		switch (state) {
			case 0:	// connection established
				memset(buffer,'\0',SIZE);
				timeout.tv_usec = 50000;
				FD_SET(sock,&fdset1);
				select_num = select(FD_SETSIZE,&fdset1,NULL,NULL,&timeout);  // waiting for message from the client
				if(select_num<0){
					perror("case 0 Select - manage_clients_control - Failed");
					state = 6;
				}
				else if(FD_ISSET(sock,&fdset1)){	//message arrived from client
					recv_data=recv(sock,buffer,SIZE,0);
					if(recv_data<0){
						perror("Reading - manage_clients_control - Failed");
						close(sock);
						exit(EXIT_FAILURE);
					}
					if (buffer[0] == 0){		// maybe second 'hello'
						if(recv_data == 0){	// no data -> the client leave the server
							state = 6;
							printf("***Client leave the server***\n");
							break;
						}
						state = 3;		// Invalid
						Invalid.replyType = 3;
						char message[36] = "Two Hello Message, Close connection";
						Invalid.replyStringSize = 36;
						Invalid.replyString = (char*)malloc(36*1);	// free in case 3
						strcpy(Invalid.replyString,message);
						break;
					}
					else if(buffer[0]==1){
						if(recv_data < 3){
							state = 3;	// Invalid command -> go to case 3
							Invalid.replyType = 3;
							char message[43] = "AskSong Message to short, Close connection";
							Invalid.replyStringSize = 43;
							Invalid.replyString = (char*)malloc(43*1);
							strcpy(Invalid.replyString,message);
							printf("%s\n",Invalid.replyString);
							break;
						}
						if(recv_data > 3){
							state = 3;	// Invalid command -> go to case 3
							Invalid.replyType = 3;
							char message[43] = "AskSong Message to large, Close connection";
							Invalid.replyStringSize = 43;
							Invalid.replyString = (char*)malloc(43*1);
							strcpy(Invalid.replyString,message);
							printf("%s\n",Invalid.replyString);
							break;
						}						
						state=1;		// announce
						break;
					}
					else if(buffer[0]==2){
						state=2;		// permit
						break;
					}		
					else {state = 5;}	
				}			
				break;

			case 1:	// got AskSong message
				station_request = buffer[1] << 8;
				station_request += buffer[2];
				if(station_request >= welcome.numStations){//if the station is not in range
					state = 3;		// Invalid
					Invalid.replyType = 3;
					char message[51] = "The station given does not exist, Close connection";
					Invalid.replyStringSize = 51;
					Invalid.replyString = (char*)malloc(51*1);	// free in case 3
					strcpy(Invalid.replyString,message);
					break;
				}
				announce.replyType = 1;
				announce.songNameSize = station_names[station_request].songNameSize;
				announce.songName = (char*)malloc(1*announce.songNameSize);
				strcpy(announce.songName,station_names[station_request].songName);
				memset(buffer,'\0',SIZE);
				buffer[0] = announce.replyType;
				buffer[1] = announce.songNameSize;
				strcpy(buffer+2,announce.songName);
				send_data = send(sock,buffer,2+announce.songNameSize,0);	// sending Announce message
				if(send_data<0){
					perror("case 1 ANNOUNCE command Failed");
					state = 6;
				}
				free(announce.songName);
				state = 0;
				break;

			case 2:	// got UpSong message
				memcpy(&songsize_upsong,buffer+1,4);
				up_song.songSize = ntohl(songsize_upsong);
				up_song.songNameSize = buffer[5];
				if(recv_data < 6 + up_song.songNameSize){
					state = 3;	// Invalid command -> go to case 3
					Invalid.replyType = 3;
					char message[42] = "UpSong Message to short, Close connection";
					Invalid.replyStringSize = 42;
					Invalid.replyString = (char*)malloc(42*1);
					strcpy(Invalid.replyString,message);
					printf("%s\n",Invalid.replyString);
					break;
				}
				if(recv_data > 6 + up_song.songNameSize){
					state = 3;	// Invalid command -> go to case 3
					Invalid.replyType = 3;
					char message[42] = "UpSong Message to large, Close connection";
					Invalid.replyStringSize = 42;
					Invalid.replyString = (char*)malloc(42*1);
					strcpy(Invalid.replyString,message);
					printf("%s\n",Invalid.replyString);
					break;
				}
				up_song.songName = (char*)malloc(up_song.songNameSize);  // free in case 4 -> NewStations
				strcpy(up_song.songName,buffer+6);
				int a = up_song.songNameSize-4;
				strcpy(mp3_check,up_song.songName + a);	// checks if the name of the file from the user input is .mp3
				if(strcmp(mp3,mp3_check)){
					permit_flag = 0;
				}
				for(i=0; i<welcome.numStations; i++){	//check if station exist
					if(!strcmp(up_song.songName,station_names[i].songName)){
						permit_flag=0;
						break;	// exit for loop
					}
				}
				if(up_song.songSize<2000){//if the song is to snall
					state = 3;		// Invalid
					Invalid.replyType = 3;
					char message[44] = "The song size is to small, Close connection";
					Invalid.replyStringSize = 44;
					Invalid.replyString = (char*)malloc(44*1);	// free in case 3
					strcpy(Invalid.replyString,message);
					break;
				}
				if(up_song.songSize>10485760){//if the song is to large
					state = 3;		// Invalid
					Invalid.replyType = 3;
					char message[44] = "The song size is to large, Close connection";
					Invalid.replyStringSize = 44;
					Invalid.replyString = (char*)malloc(44*1);	// free in case 3
					strcpy(Invalid.replyString,message);
					break;
				}//
				if(someone_sending){permit_flag=0;}	//checks if somone already sending
				if(pthread_mutex_trylock(&mutex_up)==0){ // try to lock mutex
					if(!someone_sending){		// if somone sending dont change flag
						someone_sending = 1;
					}
					pthread_mutex_unlock(&mutex_up); // unlock mutex
				}
				else {	
					permit_flag = 0; // if cant lock mutex someone try to change the flag
				}
				
				if(permit_flag){
					send_data = send(sock,permit1,2,0);	// sending UpSong message
					if(send_data<0){
						perror("case 2 sending permit 1 Failed");
						state = 6;
					}
				}
				else {
					send_data = send(sock,permit0,2,0);	// sending UpSong message
					state = 0;
					if(send_data<0){
						perror("case 2 sending permit 0 Failed");
						state = 6;
					}
					permit_flag = 1;
					break;	// exit case
				}
				
				new_UpSong = fopen(up_song.songName,"r");	// try open an exitsting file
				int flag_new = 0;				// flag for if there is no file with that name | if 0 -> there is; if 1 -> there isn't
				if(new_UpSong == NULL){				// there is no file with that name
					new_UpSong = fopen(up_song.songName,"w");
					flag_new = 1;
				}
				int byte_sent = 0;	// how much total byts sent from the client
				printf("***New song arriving!***\n");
				while(byte_sent < up_song.songSize){					
					memset(buffer,'\0',SIZE);
					FD_SET(sock,&fdset1);
					select_num = select(FD_SETSIZE,&fdset1,NULL,NULL,&upsong_timeout);  // waiting for message from the client
					if(select_num<0){
						perror("case 2 select - manage_clients_control - Failed");
						state = 6;
					}
					else if(select_num == 0){
						printf("case 2 timeout upsong send invalid command\n"); 
						state = 3;		// case 3 -> InvalidCommand
						Invalid.replyType = 3;
						char message[33] = "UpSong timeout, Close connection";
						Invalid.replyStringSize = 33;
						Invalid.replyString = (char*)malloc(33*1);	// free in case 3
						strcpy(Invalid.replyString,message);
						break;	// exit while
					}
					else if(FD_ISSET(sock,&fdset1)){
						recv_data=recv(sock,buffer,KIB,0);
						if(recv_data<0){
							perror("case 2 Reading - manage_clients_control - Failed");
							close(sock);
							exit(EXIT_FAILURE);
						}
						if(flag_new = 1){fwrite(buffer,1,recv_data,new_UpSong);}// only if the file dont exist write to it
						byte_sent += recv_data;
					}
					upsong_timeout.tv_sec = 3;
				}
				if(byte_sent == up_song.songSize){	// sending 'NewStations Message' only if recived all data
					state = 4;
					printf("***New song fully arrived!***\n");
				}	
				fclose(new_UpSong);
				someone_sending = 0;	// allow other clients to upload a song
				break;

			case 3:	// Invalid command 
				buffer[0] = Invalid.replyType;	
				buffer[1] = Invalid.replyStringSize;
				strcpy(buffer+2,Invalid.replyString);
				send_data = send(sock,buffer,2+Invalid.replyStringSize,0);	// sending UpSong message
				if(send_data<0){
					perror("case 3 invalid command Failed");
					state = 6;
				}
				free(Invalid.replyString);
				state = 6;
				break;

			case 4:	// send to all clients NewStations message
				welcome.numStations++;
				station_names = (struct Station*)realloc(station_names, sizeof(struct Station)*welcome.numStations);
				int num = welcome.numStations - 1;
				i = welcome.numStations - 1;
				int j;
				for(j=0; j<4 ;j++){
					station_names[i].multicastGroup[j] = welcome.multicastGroup[j];
				}
				IP_Check(i);	// check & fix ip -> x.1.2.3	
				station_names[i].songNameSize = up_song.songNameSize;
				station_names[i].songName = (char*)malloc(sizeof(char)*up_song.songNameSize);
				strcpy(station_names[i].songName,up_song.songName);
				pthread_create(&station_list[i], NULL, &play_song, (void*) &num);	// open thread to the station	
				new_stations.replyType = 4;
				new_stations.newStationNumber = htons(welcome.numStations);
				memset(buffer,'\0',SIZE);
				buffer[0] =  new_stations.replyType;
				memcpy(buffer+1,&new_stations.newStationNumber,2);			
				for(i=0; i<num_clients; i++){
					send_data = send(client_socket[i],buffer,3,0);	// sending UpSong message
					if(send_data<0){
						perror("case 4 sending new stations Failed");
						state = 6;
					}
				}
				state = 0;
				break;

			case 5:	// Error - wrong message
				printf("case 5 wrong message\n");
				state = 6;		// send to close 		
				break;

			case 6:	// Error - general
				local_running = 0;	// flag to 0 -> wont run in the running while loop
				close(sock);
				client_socket[index] = 0;
				num_clients--;
				break;
		} // end of switch	
	}
	pthread_exit(&client_list[index]);
}


int OpenWelcomeSocket(uint16_t port){
	int serverSocket, option = 1;
	struct sockaddr_in serverAddr;
	socklen_t server_size;

	serverSocket = socket(PF_INET, SOCK_STREAM, 0);
	if(serverSocket <0){
		perror("Socket Failed");
		close(serverSocket);
		return -1;
	}

	if(setsockopt(serverSocket,SOL_SOCKET,SO_REUSEADDR | SO_REUSEPORT,&option, sizeof(option))){
		perror("Socket Option Failed");
		close(serverSocket);
		return -1;
	}

	serverAddr.sin_family = AF_INET;				// address family - IPv4
	serverAddr.sin_port = htons(port);				// which port
	serverAddr.sin_addr.s_addr = INADDR_ANY;			// IP address
	memset(serverAddr.sin_zero, '\0', sizeof serverAddr.sin_zero);	// padding field to 0
	server_size = sizeof(serverAddr);

	if((bind(serverSocket, (struct sockaddr *) &serverAddr, server_size))<0){
		perror("Bind Failed");
		close(serverSocket);
		return -1;
	}
	if(listen(serverSocket,SOMAXCONN) == 0){
		//printf("Listening...\n");
	}	
	else{
		perror("Listen Failed");
		close(serverSocket);
		return -1;
	}

return serverSocket;
}







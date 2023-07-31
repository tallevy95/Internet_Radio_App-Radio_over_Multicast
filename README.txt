Simple internet radio which implemented with simple connection protocol and multicast transmission.
The app uses TCP and UDP.

1. To play music you need to have mp3 files in app directory.
2. App was made for Linux OS.
3. The app was made for GNS3.
4. FSM of client/server and Makefile are in repository too.

//////////////////////////////////////////////////////////////////////////////////////////////

Protocol use the following messages:

#Client to Server:
  Hello:
    uint8_t commandType = 0;
    uint16_t reserved = 0;
    uint8_t hello_pckt[3];
    uint8_t commandType;

  AskSong:
    uint8_t commandType = 1;
    uint16_t stationNumber;

  UpSong:
    uint8_t commandType = 2;
    uint32_t songSize; //in bytes
    uint8_t songNameSize;
    char songName[songNameSize];

#Server to Client:
  Welcome:
    uint8_t replyType = 0;
    uint16_t numStations;
    uint32_t multicastGroup;
    uint16_t portNumber;

  Announce:
    uint8_t replyType = 1;
    uint8_t songNameSize;
    char songName[songNameSize];

  PermitSong:
    uint8_t replyType = 2;
    uint8_t permit;

  InvalidCommand:
    uint8_t replyType = 3;
    uint8_t replyStringSize;
    char replyString[replyStringSize];

  NewStations:
    uint8_t replyType = 4;
    uint16_t newStationNumber

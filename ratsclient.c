#include <stdio.h>      // for fprintf, printf, FILE, fgets, fflush, fdopen, setvbuf, etc.
#include <stdlib.h>     // for exit, malloc, free
#include <string.h>     // for strlen, memset
#include <sys/types.h>  // for socket types
#include <sys/socket.h> // for socket(), connect(), etc.
#include <netdb.h>      // for getaddrinfo(), freeaddrinfo(), struct addrinfo
#include <unistd.h>     // for close(), dup()
#include <csse2310.h>

// Function prototypes
void validate_arguments(int argc, char *argv[]);
int check_and_connect_port(const char *port);
FILE* setup_server_streams(int sockfd, FILE **serverOut);
void send_client_info(FILE *serverOut, const char *clientName, const char *gameName);
void display_hand(const Hand* hand);

#define MAX_CARDS 13
#define CARD_LEN 4

typedef struct {
    char cards[MAX_CARDS][CARD_LEN];
    int count;
} Hand;

// Validate command line arguments
void validate_arguments(int argc, char *argv[]) {
    if (argc != 4) {
        fprintf(stderr, "Usage: ./ratsclient clientname game port\n");
        exit(3);
    }
    for (int i = 1; i < 4; i++) {
        if (strlen(argv[i]) == 0) {
            fprintf(stderr, "ratsclient: invalid arguments\n");
            exit(20);
        }
    }
}

/**
 * Attempts to connect to the given port on localhost.
 * If the connection cannot be established, prints an error and exits with code 5.
 */
int check_and_connect_port(const char *port) {
    struct addrinfo addrHints, *resolvedAddrs, *addrPtr;
    int connectionFd = -1;

    memset(&addrHints, 0, sizeof(addrHints));
    addrHints.ai_family = AF_UNSPEC;      // IPv4 or IPv6
    addrHints.ai_socktype = SOCK_STREAM;  // TCP connection

    int status = getaddrinfo("localhost", port, &addrHints, &resolvedAddrs);
    if (status != 0) {
        fprintf(stderr, "ratsclient: unable to connect to the server\n");
        exit(5);
    }

    for (addrPtr = resolvedAddrs; addrPtr != NULL; addrPtr = addrPtr->ai_next) {
        connectionFd = socket(addrPtr->ai_family, addrPtr->ai_socktype, addrPtr->ai_protocol);
        if (connectionFd == -1)
            continue;

        if (connect(connectionFd, addrPtr->ai_addr, addrPtr->ai_addrlen) == 0)
            break;  // success
        
        //free failure
        close(connectionFd);
        connectionFd = -1;
    }

    freeaddrinfo(resolvedAddrs);

    //check all connection fail
    if (connectionFd == -1) {
        fprintf(stderr, "ratsclient: unable to connect to the server\n");
        exit(5);
    }

    return connectionFd;
}

// Setup server input and output streams
FILE* setup_server_streams(int sockfd, FILE **serverOut) {
    FILE *serverIn = fdopen(sockfd, "r");
    if (!serverIn) {
        fprintf(stderr, "ratsclient: unable to connect to the server\n");
        close(sockfd);
        exit(5);
    }
    
    int output_file_descriptor = dup(sockfd);
    if (output_file_descriptor == -1) {
        fprintf(stderr, "ratsclient: unable to connect to the server\n");
        fclose(serverIn);
        exit(5);
    }
    
    *serverOut = fdopen(output_file_descriptor, "w");
    if (!*serverOut) {
        fprintf(stderr, "ratsclient: unable to connect to the server\n");
        fclose(serverIn);
        close(output_file_descriptor);
        exit(5);
    }
    
    setvbuf(*serverOut, NULL, _IOLBF, 0);
    return serverIn;
}

// Send client name and game name to server
/**
 * ratsclient should send the user’s name and game name as soon as it connects to ratsserver.
 *  The client informs the server of the user’s name and requested game name by sending them to the server on individual lines, 
 * e.g. Harry\nExplodingSnap\n 
 */
void send_client_info(FILE *serverOut, const char *clientName, const char *gameName) {
    if (fprintf(serverOut, "%s\n", clientName) < 0 || fflush(serverOut) != 0 ||
        fprintf(serverOut, "%s\n", gameName) < 0 || fflush(serverOut) != 0) {
        fprintf(stderr, "ratsclient: unable to connect to the server\n");
        fclose(serverOut);
        exit(5);
    }
}

void display_cards(const Hand* hand, char type) {
    for (int i = 0; i < hand->count; i++)
        if (hand->cards[i][strlen(hand->cards[i]) - 1] == type)
            printf(" %.*s", (int)(strlen(hand->cards[i]) - 1), hand->cards[i]);
}
void display_hand(const Hand* hand) {
    printf("S:");
    display_cards(hand, 'S');

    printf("\nC:");
    display_cards(hand, 'C');

    printf("\nD:");
    display_cards(hand, 'D');

    printf("\nH:");
    display_cards(hand, 'H');
    printf("\n");

}


void parse_hand_message(const char* message, Hand* hand) {
    hand->count = 0;
    const char* ptr = message + 1;
    while(*ptr) {
        while(*ptr == ' ') ptr ++;
        if(*ptr == '\n' || *ptr == '\0') break;
        sscanf(ptr, "%3s", hand->cards[hand->count]);
        hand->count++;
        while(*ptr && *ptr != ' ') ptr++;
    }
}





// Main function - orchestrates the client execution
int main(int argc, char *argv[]) {
    // 1. Parse and validate arguments
    validate_arguments(argc, argv);
    char *clientName = argv[1];
    char *gameName   = argv[2];
    char *port       = argv[3];

    // 2. Connect to server
    int sockfd = check_and_connect_port(port);
    
    // 3. Setup server streams
    FILE *serverOut;
    FILE *serverIn = setup_server_streams(sockfd, &serverOut);
    

    
    return 0;
}
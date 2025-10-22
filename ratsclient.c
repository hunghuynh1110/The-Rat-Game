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
void run_game_loop(FILE *serverIn, FILE *serverOut);
void handle_message(const char *buf, FILE *serverIn, FILE *serverOut);

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

// Handle different types of messages from server
void handle_message(const char *message_buffer, FILE *serverIn, FILE *serverOut) {
    if (message_buffer[0] == 'M') {
        // Info message
        printf("Info: %s", message_buffer + 1);
        if (message_buffer[strlen(message_buffer)-1] != '\n') printf("\n");
    } else if (message_buffer[0] == 'H') {
        // Hand message
        printf("%s", message_buffer + 1);
        if (message_buffer[strlen(message_buffer)-1] != '\n') printf("\n");
    } else if (message_buffer[0] == 'O') {
        // Game over
        fclose(serverIn);
        fclose(serverOut);
        exit(0);
    } else {
        fprintf(stderr, "ratsclient: a protocol error occurred\n");
        fclose(serverIn);
        fclose(serverOut);
        exit(7);
    }
}

// Main game loop - receive and process messages from server
void run_game_loop(FILE *serverIn, FILE *serverOut) {
    char message_buffer[4096];
    while (fgets(message_buffer, sizeof(message_buffer), serverIn)) {
        handle_message(message_buffer, serverIn, serverOut);
    }
    // If server closes connection unexpectedly, treat as protocol error
    fprintf(stderr, "ratsclient: a protocol error occurred\n");
    fclose(serverIn);
    fclose(serverOut);
    exit(7);
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
    
    // 4. Send client information
    send_client_info(serverOut, clientName, gameName);

    // 5. Run game loop
    run_game_loop(serverIn, serverOut);
    
    return 0;
}
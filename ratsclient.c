#include <stdio.h>      // for fprintf, printf, FILE, fgets, fflush, fdopen, setvbuf, etc.
#include <stdlib.h>     // for exit, malloc, free
#include <string.h>     // for strlen, memset
#include <sys/types.h>  // for socket types
#include <sys/socket.h> // for socket(), connect(), etc.
#include <netdb.h>      // for getaddrinfo(), freeaddrinfo(), struct addrinfo
#include <unistd.h>     // for close(), dup()

#define MAX_CARDS 13
#define CARD_LEN 4

typedef struct {
    char cards[MAX_CARDS][CARD_LEN];
    int count;
    char lastSend[3];
} Hand;


// Function prototypes
void validate_arguments(int argc, char *argv[]);
int check_and_connect_port(const char *port);
FILE* setup_server_streams(int sockfd, FILE **serverOut);
void send_client_info(FILE *serverOut, const char *clientName, const char *gameName);

void display_cards(const Hand* hand, char type);
void display_hand(const Hand* hand);

void parse_hand_message(const char* message, Hand* hand);
void handle_message(const char *message, FILE* serverOut, Hand* hand);

void handle_lead(FILE *serverOut, Hand *hand);
void handle_play(FILE *serverOut, Hand *hand, char leadSuit);
void handle_accept(Hand *hand);

//helper funct
int get_rank_value(char rank);
int get_suit_value(char suit);
int compare_cards(const void *a, const void *b);
static void remove_card_from_hand(Hand *hand, const char rs[3]);



int get_rank_value(char rank) {
    if (rank >= '2' && rank <= '9') {
        return rank - '0';
    }
    switch (rank) {
        case 'A': return 14;
        case 'K': return 13;
        case 'Q': return 12;
        case 'J': return 11;
        case 'T': return 10;
        default: return 0; // Should not happen
    }
}

int get_suit_value(char suit) {
    switch (suit) {
        case 'S': return 0;
        case 'C': return 1;
        case 'D': return 2;
        case 'H': return 3;
        default:
            return 4;
        }
}

int compare_cards(const void *a, const void *b) {
    const char *cardA = *(const char (*)[CARD_LEN])a;
    const char *cardB = *(const char (*)[CARD_LEN])b;

    // Suit is at index 1, Rank is at index 0
    char suitA = cardA[1];
    char suitB = cardB[1];

    int suitValA = get_suit_value(suitA);
    int suitValB = get_suit_value(suitB);

    if (suitValA != suitValB) {
        return suitValA - suitValB; // Sort by suit order (S, C, D, H)
    }

    // Suits are the same, sort by rank in decreasing order
    char rankA = cardA[0];
    char rankB = cardB[0];

    int rankValA = get_rank_value(rankA);
    int rankValB = get_rank_value(rankB);

    return rankValB - rankValA; // Decreasing order: larger ranks first
}

static void remove_card_from_hand(Hand *hand, const char rs[3]) {
    if (!hand || !rs || rs[0] == '\0' || rs[1] == '\0') return;

    for (int i = 0; i < hand->count; ++i) {
        char c0 = hand->cards[i][0];
        char c1 = hand->cards[i][1];

        // Match either rank-suit or (temporarily) suit-rank
        if ((c0 == rs[0] && c1 == rs[1]) || (c0 == rs[1] && c1 == rs[0])) {
            // Shift left
            for (int j = i; j < hand->count - 1; ++j) {
                memcpy(hand->cards[j], hand->cards[j + 1], CARD_LEN);
            }
            memset(hand->cards[hand->count - 1], 0, CARD_LEN);
            hand->count--;
            break;
        }
    }
}


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
    for (int i = 0; i < hand->count; i++) {
        // Card is stored as "RankSuit" (rank at index 0, suit at index 1)
        // Display as "SuitRank" (e.g., "SA")
        if (hand->cards[i][1] == type) {
            // Print suit then rank
            printf(" %c", hand->cards[i][0]);
        }
    }
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

//done
void parse_hand_message(const char* message, Hand* hand) {
    hand->count = 0;
    const char* ptr = message + 1; // Skip 'H' or the ' ' after H
    
    while (*ptr) {
        while (*ptr == ' ') ptr++; // Skip leading spaces
        if (*ptr == '\n' || *ptr == '\0') break;
        
        // Cards from server are in format "SuitRank" (e.g., "SA", "D2")
        // We store them as "RankSuit" (rank at index 0, suit at index 1)
        // First char from server is suit, second is rank
        hand->cards[hand->count][0] = *ptr++; // rank first
        hand->cards[hand->count][1] = *ptr++; // suit second
        hand->cards[hand->count][2] = '\0';   // Null-terminate
        
        hand->count++;
        
        // Skip any spaces after this card
        while (*ptr && *ptr == ' ') ptr++;
    }

    // Sort the hand now that it's fully parsed
    qsort(hand->cards, hand->count, CARD_LEN, compare_cards);
}

void handle_lead(FILE *serverOut, Hand *hand) {
    display_hand(hand);
    printf("Lead> ");
    fflush(stdout);

    char input[16];
    if (fgets(input, sizeof(input), stdin) == NULL) {
        fprintf(stderr, "ratsclient: user has quit\n");
        exit(17);
    }

    input[strcspn(input, "\n")] = '\0';

    // Capture the first token and remember first two chars as the card we intend
    char token[8] = {0};
    if (sscanf(input, " %7s", token) == 1 && strlen(token) >= 2) {
        hand->lastSend[0] = token[0];
        hand->lastSend[1] = token[1];
        hand->lastSend[2] = '\0';
        fprintf(serverOut, "%s\n", token);
    } else {
        hand->lastSend[0] = '\0';
        fprintf(serverOut, "%s\n", input);
    }
    fflush(serverOut);

}

void handle_play(FILE *serverOut, Hand *hand, char leadSuit) {
    display_hand(hand);
    printf("[%c] play> ", leadSuit);
    fflush(stdout);

    char card[10];
    if (fgets(card, sizeof(card), stdin) == NULL) {
        fprintf(stderr, "ratsclient: user has quit\n");
        exit(17);
    }

    card[strcspn(card, "\n")] = '\0';
    if (strlen(card) >= 2) {
        hand->lastSend[0] = card[0];
        hand->lastSend[1] = card[1];
        hand->lastSend[2] = '\0';
    } else {
        hand->lastSend[0] = hand->lastSend[1] = hand->lastSend[2] = '\0';
    }
    fprintf(serverOut, "%s\n", card);
    fflush(serverOut);
}

void handle_accept(Hand *hand) {
    if (hand->lastSend[0] != '\0' && hand->lastSend[1] != '\0') {
        remove_card_from_hand(hand, hand->lastSend);   // <- fixed name
        hand->lastSend[0] = '\0';
        hand->lastSend[1] = '\0';
        hand->lastSend[2] = '\0';
    }
}

void handle_message(const char *message, FILE* serverOut, Hand* hand) {
    switch (message[0]) {
        case 'M':
            printf("Info: %s", message+1);
            break;
        case 'A':
            /**
             * In successive tricks, cards that have been played should be removed. 
             * Do not remove a card until the server has sent an “A” message
             * */
            handle_accept(hand);
            break;
        case 'L':
            /**
             * When the game starts, the client will wait for the server to ask it to play. 
             * If it has the lead, then the hand should be displayed followed by the prompt
             *          Lead>
             */
            handle_lead(serverOut, hand);
            break;
        case 'H':
            /**
             * If the client does not have the lead, then it will display the hand followed by the prompt
             *          H <card1> <card2> ... <cardN>\n
             */
            parse_hand_message(message, hand);
            display_hand(hand);
            break;
        case 'P': {
            /**
             * Tells the client to play a card following the lead suit <suit>.
             * [<suit>] play>
             */
            char suit;
            if (sscanf(message, "P %c", &suit) != 1 && sscanf(message, "P%c", &suit) != 1) {
                fprintf(stderr, "ratsclient: a protocol error occurred\n");
                exit(7);
            }
            // sanity-check the suit
            if (suit != 'S' && suit != 'C' && suit != 'D' && suit != 'H') {
                fprintf(stderr, "ratsclient: a protocol error occurred\n");
                exit(7);
            }

            handle_play(serverOut, hand, suit);
            break;
        }
        case 'O':
            //end game
            exit(0);
            break;

        default:
            fprintf(stderr, "ratsclient: a protocol error occurred\n");
            exit(7);
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

    send_client_info(serverOut, clientName, gameName);
    
    Hand hand;
    char messageBuffer[256];
    memset(&hand, 0, sizeof(hand));

    while (fgets(messageBuffer, sizeof(messageBuffer), serverIn)) {
        handle_message(messageBuffer, serverOut, &hand);
    }
    fprintf(stderr, "ratsclient: a protocol error occurred\n");
    exit(7);

    
    return 0;
}

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
} Hand;

typedef struct {
    char lastCard;
} lastCard;

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
void handle_accept(Hand *hand, const char *card);

//helper funct
int get_rank_value(char rank);
int get_suit_value(char suit);
int compare_cards(const void *a, const void *b);

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

    // Rank is at index 0, Suit is at index 1
    char suitA = cardA[1];
    char suitB = cardB[1];

    int suitValA = get_suit_value(suitA);
    int suitValB = get_suit_value(suitB);

    if (suitValA != suitValB) {
        return suitValA - suitValB; // Sort by suit order
    }

    // Suits are the same, sort by rank (index 0)
    char rankA = cardA[0];
    char rankB = cardB[0];

    int rankValA = get_rank_value(rankA);
    int rankValB = get_rank_value(rankB);

    return rankValB - rankValA; // b-a = decreasing order
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
        // Card is 2 chars, e.g., "SA". Suit is at index 1.
        if (hand->cards[i][1] == type) {
            // Rank is at index 0.
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


void parse_hand_message(const char* message, Hand* hand) {
    hand->count = 0;
    const char* ptr = message + 1; // Skip 'H'
    
    while (*ptr) {
        while (*ptr == ' ') ptr++; // Skip spaces
        if (*ptr == '\n' || *ptr == '\0') break;
        
        // Cards are always 2 chars, e.g., "SA", "ST", "S2"
        sscanf(ptr, "%2s", hand->cards[hand->count]); 
        hand->cards[hand->count][2] = '\0'; // Null-terminate at index 2
        hand->count++;
        
        // Move ptr to the next space or end
        // Move 2 chars for the card we just read
        ptr += 2;
        while (*ptr && *ptr == ' ') ptr++; // Skip trailing spaces
    }

    // Sort the hand now that it's fully parsed
    qsort(hand->cards, hand->count, CARD_LEN, compare_cards);
}

void handle_lead(FILE *serverOut, Hand *hand) {
    display_hand(hand);
    printf("Lead> ");
    fflush(stdout);

    char card[10];
    if (fgets(card, sizeof(card), stdin) == NULL) {
        fprintf(stderr, "ratsclient: user has quit\n");
        exit(17);
    }

    card[strcspn(card, "\n")] = '\0'; // remove newline
    fprintf(serverOut, "%s\n", card);
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
    fprintf(serverOut, "%s\n", card);
    fflush(serverOut);
}

void handle_accept(Hand *hand, const char *card) {
    char cleanCard[10];
    sscanf(card, "%3s", cleanCard);

    for (int i = 0; i < hand->count; i++) {
        if (strcmp(hand->cards[i], cleanCard) == 0) {
            // Shift remaining cards left
            for (int j = i; j < hand->count - 1; j++) {
                strcpy(hand->cards[j], hand->cards[j + 1]);
            }
            hand->count--;
            break;
        }
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
            handle_accept(hand, message+2);
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
        case 'P':
            /**
             * Tells the client to play a card following the lead suit <suit>.
             * [<suit>] play>
             */
            handle_play(serverOut, hand, message[2]);
            break;
        case 'O':
            //end game
            exit(0);
            break;

        default:
            fprintf(stderr, "ratsclient: a protocol error occurred\n");
            exit(7);
    }
}


/**
 * A temporary function to test hand parsing and sorting.
 */
void test_hand_sorting() {
    printf("--- HAND SORTING TEST ---\n");
    Hand testHand;

    // Test Case 1: Unsorted ranks and a 'T' card
    const char* msg1 = "H S2 SA D5 D9 CT CA";
    printf("\nTest 1 Input: %s\n", msg1);
    printf("Expected:\n");
    printf("S: SA S2\nC: CA CT\nD: D9 D5\nH:\n");
    printf("Actual:\n");
    parse_hand_message(msg1, &testHand);
    display_hand(&testHand);

    // Test Case 2: Unsorted suits and ranks
    const char* msg2 = "H HA S2 D9 C3 D5 SA";
    printf("\nTest 2 Input: %s\n", msg2);
    printf("Expected:\n");
    printf("S: SA S2\nC: C3\nD: D9 D5\nH: HA\n");
    printf("Actual:\n");
    parse_hand_message(msg2, &testHand);
    display_hand(&testHand);

    printf("\n--- END OF TEST ---\n");
    exit(99); // Exit so client doesn't try to connect
}

// Main function - orchestrates the client execution
int main(int argc, char *argv[]) {
    test_hand_sorting(); // <-- TEMPORARY TEST CALL
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
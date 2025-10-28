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
static int card_in_hand(const Hand *hand, char r, char s);


/**
 * get_rank_value
 * --------------
 * Converts a rank character ('2'..'9', 'T', 'J', 'Q', 'K', 'A') into a
 * numeric value suitable for comparisons and sorting.
 *
 * Parameters:
 *   rank - rank character to convert.
 *
 * Returns:
 *   Integer value where higher means stronger rank (e.g., 2=2 ... A=14).
 *   Returns -1 if the rank is invalid.
 *
 * Notes:
 *   This is a display/sorting helper and does not enforce any game rule
 *   beyond a total order of ranks.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
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

/**
 * get_suit_value
 * --------------
 * Maps a suit character ('S', 'H', 'D', 'C') to a deterministic integer
 * order for client-side sorting and display. The exact order is an internal
 * UI choice and has no effect on game legality.
 *
 * Parameters:
 *   suit - suit character to convert.
 *
 * Returns:
 *   Non-negative integer indicating suit order (lower comes first), or -1
 *   if the suit character is invalid.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
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

/**
 * compare_cards
 * -------------
 * qsort-compatible comparator for sorting card tokens by suit and rank using
 * get_suit_value() and get_rank_value(). The intended order is stable within
 * a suit and deterministic across suits for a neat hand display.
 *
 * Parameters:
 *   a, b - pointers to two card elements (implementation-defined storage),
 *          each representing a rank/suit pair.
 *
 * Returns:
 *   <0 if *a should come before *b,
 *    0 if they are equivalent in the chosen order,
 *   >0 if *a should come after *b.
 *
 * Preconditions:
 *   Card tokens referenced by a and b must contain valid rank/suit chars.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
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

/**
 * remove_card_from_hand
 * ---------------------
 * Removes a specific card from the Hand if present, compacting the array to
 * fill the gap. This is typically called after receiving server 'A' (accept).
 *
 * Parameters:
 *   hand - Hand to mutate (must not be NULL).
 *   rs   - 2-character rank/suit string (e.g., "TD") with trailing '\0'.
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   Decrements hand->count on success and shifts following elements left by
 *   one position. If the card is not found, the Hand is unchanged.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void remove_card_from_hand(Hand *hand, const char rs[3]) {
    if (!hand || !rs || rs[0] == '\0' || rs[1] == '\0') return;

    for (int i = 0; i < hand->count; ++i) {
        char c0 = hand->cards[i][0];
        char c1 = hand->cards[i][1];

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

/**
 * card_in_hand
 * ------------
 * Searches the Hand for a given rank/suit pair.
 *
 * Parameters:
 *   hand - Hand to inspect (must not be NULL).
 *   r    - rank character to find.
 *   s    - suit character to find.
 *
 * Returns:
 *   Zero-based index of the matching card if found; otherwise -1.
 *
 * Complexity:
 *   O(n) over the current number of cards (n = hand->count).
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static int card_in_hand(const Hand *hand, char r, char s) {
    if (!hand) return 0;
    for (int i = 0; i < hand->count; ++i) {
        if (hand->cards[i][0] == r && hand->cards[i][1] == s) {
            return 1;
        }
    }
    return 0;
}


/**
 * validate_arguments
 * ------------------
 * Validates command-line arguments and exits with the correct usage/status
 * if they are invalid (per assignment specification).
 *
 * Parameters:
 *   argc - count of command-line arguments.
 *   argv - vector of command-line argument strings.
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   Prints usage or error messages to stderr and may terminate the program
 *   with the specified exit status on invalid input.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
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
 * check_and_connect_port
 * ----------------------
 * Resolves and connects to the server on the given port/service.
 *
 * Parameters:
 *   port - C string containing the port number or service name to connect to.
 *
 * Returns:
 *   Connected socket file descriptor (non-negative) on success;
 *   on failure, prints the appropriate error message and terminates
 *   with the specified exit status (per spec), or returns a negative value
 *   if the caller is expected to handle errors.
 *
 * Notes:
 *   The exact exit behaviour should match the assignment’s error-handling
 *   table (e.g., invalid port vs. connect/bind/listen failures).
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
int check_and_connect_port(const char *port) {
    struct addrinfo addrHints, *resolvedAddrs, *addrPtr;
    int connectionFd = -1;

    memset(&addrHints, 0, sizeof(addrHints));
    addrHints.ai_family = AF_UNSPEC;
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

/**
 * setup_server_streams
 * --------------------
 * Wraps a connected socket in standard I/O streams for line-oriented
 * protocol I/O and configures buffering as required.
 *
 * Parameters:
 *   sockfd    - connected socket file descriptor.
 *   serverOut - output parameter; on success, *serverOut is set to a FILE*
 *               opened for writing to the server.
 *
 * Returns:
 *   FILE* opened for reading from the server (input stream) on success;
 *   NULL on failure (and may close sockfd as needed).
 *
 * Side effects:
 *   May dup() the socket and adjust stream buffering. Caller is responsible
 *   for closing the returned FILE* streams when finished.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
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

/**
 * send_client_info
 * ----------------
 * Sends the client’s identity and target game name to the server according
 * to the protocol (one line per field, newline-terminated).
 *
 * Parameters:
 *   serverOut  - writable FILE* connected to the server.
 *   clientName - client/player name to send.
 *   gameName   - game name (lobby/room) to send.
 *
 * Returns:
 *   None.
 *
 * Preconditions:
 *   serverOut must be non-NULL and writable; strings must be non-NULL.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
void send_client_info(FILE *serverOut, const char *clientName, const char *gameName) {
    if (fprintf(serverOut, "%s\n", clientName) < 0 || fflush(serverOut) != 0 ||
        fprintf(serverOut, "%s\n", gameName) < 0 || fflush(serverOut) != 0) {
        fprintf(stderr, "ratsclient: unable to connect to the server\n");
        fclose(serverOut);
        exit(5);
    }
}

/**
 * display_cards
 * -------------
 * Renders a subset or grouping of cards from the hand based on the
 * requested type/filter (e.g., by suit or presentation mode).
 *
 * Parameters:
 *   hand - pointer to the current Hand to display.
 *   type - display selector; semantics defined by the caller (e.g., suit
 *          character 'S','H','D','C' or a mode flag).
 *
 * Returns:
 *   None.
 *
 * Notes:
 *   This function does not modify the Hand; it only prints to stdout.
 *   Call display_hand() for a full view if no filtering is desired.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
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

/**
 * display_hand
 * ------------
 * Renders the player's entire current hand to stdout in the ratsclient's
 * chosen UI/format. Intended for human readability during interactive play.
 *
 * Parameters:
 *   hand - pointer to the Hand to be displayed (must not be NULL).
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   Prints to stdout. Does not mutate the Hand contents.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
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

/**
 * parse_hand_message
 * ------------------
 * Parses an 'H' message from the server containing a 52-character card
 * payload (26 rank/suit pairs) and loads it into the client's Hand model.
 *
 * Parameters:
 *   message - null-terminated server line beginning with 'H' followed by
 *             52 card characters (must not be NULL).
 *   hand    - output Hand to populate with the 26 cards (must not be NULL).
 *
 * Returns:
 *   None.
 *
 * Preconditions:
 *   The message format must conform to the protocol; no whitespace is
 *   expected in the card payload. Rank/suit characters are assumed valid
 *   per spec.
 *
 * Side effects:
 *   Overwrites the Hand contents and resets its count to 26.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
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

/**
 * handle_lead
 * -----------
 * Handles a server 'L' prompt by obtaining a lead card from the user,
 * validating that the card exists in the Hand, and sending it to the server.
 * Re-prompts locally until a syntactically valid card that exists in the
 * Hand is entered.
 *
 * Parameters:
 *   serverOut - writable FILE* to the server (must not be NULL).
 *   hand      - player's Hand to validate against (must not be NULL).
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   Reads from stdin, writes to stdout for the prompt, and sends the chosen
 *   card (e.g., "TD" for Ten of Diamonds) on a single line to serverOut.
 *   Does not remove the card from the Hand; removal occurs upon acceptance.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
void handle_lead(FILE *serverOut, Hand *hand) {
    while (1) {
        display_hand(hand);
        printf("Lead> ");
        fflush(stdout);

        char buf[16];
        if (!fgets(buf, sizeof buf, stdin)) {
            fprintf(stderr, "ratsclient: user has quit\n");
            exit(17);
        }
        buf[strcspn(buf, "\r\n")] = '\0';

        if (strlen(buf) != 2) {
            continue; // re-prompt; do not talk to server
        }

        char r = buf[0];
        char s = buf[1];

        // Validate characters
        if (get_rank_value(r) == 0) {
            continue;
        }
        if (get_suit_value(s) > 3) { // invalid suit
            continue;
        }
        // Must exist in hand (leader has no follow-suit restriction)
        if (!card_in_hand(hand, r, s)) {
            continue;
        }

        // Send once valid
        fprintf(serverOut, "%c%c\n", r, s);
        fflush(serverOut);
        hand->lastSend[0] = r;
        hand->lastSend[1] = s;
        hand->lastSend[2] = '\0';
        return;
    }
}

/**
 * handle_play
 * -----------
 * Handles a server 'P' prompt for a follower given the lead suit. Obtains
 * a card from the user, validates syntax, presence in the Hand, and—if the
 * Hand contains any cards of leadSuit—enforces follow-suit. Will re-prompt
 * locally until a legal card is provided, then sends that card to the server.
 *
 * Parameters:
 *   serverOut - writable FILE* to the server (must not be NULL).
 *   hand      - player's Hand to validate against (must not be NULL).
 *   leadSuit  - suit character ('S','H','D','C') indicating the trick’s lead.
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   Reads from stdin, writes to stdout for prompts, and sends a single-line
 *   card token to serverOut. Does not remove the card from the Hand; removal
 *   occurs upon acceptance in handle_accept().
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
void handle_play(FILE *serverOut, Hand *hand, char leadSuit) {
    while (1) {
        display_hand(hand);
        printf("[%c] play> ", leadSuit);
        fflush(stdout);

        char buf[16];
        if (!fgets(buf, sizeof buf, stdin)) {
            fprintf(stderr, "ratsclient: user has quit\n");
            exit(17);
        }
        buf[strcspn(buf, "\r\n")] = '\0';

        if (strlen(buf) != 2) {
            continue; // re-prompt; do not talk to server
        }

        char r = buf[0];
        char s = buf[1];

        // Validate characters
        if (get_rank_value(r) == 0) {
            continue;
        }
        if (get_suit_value(s) > 3) { // invalid suit
            continue;
        }
        // Must exist in the hand
        if (!card_in_hand(hand, r, s)) {
            continue;
        }
        // If we have any of the lead suit, the chosen card must match leadSuit
        int haveLead = 0;
        for (int i = 0; i < hand->count; ++i) {
            if (hand->cards[i][1] == leadSuit) { haveLead = 1; break; }
        }
        if (haveLead && s != leadSuit) {
            continue;
        }

        // Send once valid
        fprintf(serverOut, "%c%c\n", r, s);
        fflush(serverOut);
        hand->lastSend[0] = r;
        hand->lastSend[1] = s;
        hand->lastSend[2] = '\0';
        return;
    }
}

/**
 * handle_accept
 * -------------
 * Processes an 'A' acknowledgement from the server indicating that the most
 * recent card sent by the client has been accepted as a legal play. This
 * function updates the local Hand to remove the accepted card (e.g., using
 * the client's stored "last sent" card token).
 *
 * Parameters:
 *   hand - pointer to the client's Hand to mutate (must not be NULL).
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   Mutates the Hand by removing exactly one card corresponding to the last
 *   transmitted play. If the last-sent card cannot be found, the Hand is left
 *   unchanged and the client may optionally report a protocol error.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
void handle_accept(Hand *hand) {
    if (hand->lastSend[0] != '\0' && hand->lastSend[1] != '\0') {
        remove_card_from_hand(hand, hand->lastSend);   // <- fixed name
        hand->lastSend[0] = '\0';
        hand->lastSend[1] = '\0';
        hand->lastSend[2] = '\0';
    }
}

/**
 * handle_message
 * --------------
 * Dispatches a single server line to the appropriate client handler.
 * Typical messages include:
 *   'M' ... : informational text (printed to stdout),
 *   'H' ... : initial hand (parsed via parse_hand_message()),
 *   'L'     : prompt to lead a trick (handled via handle_lead()),
 *   'P' x   : prompt to play following lead suit x (handle_play()),
 *   'A'     : acknowledgement of a valid play (handle_accept()).
 *
 * Parameters:
 *   message   - the raw server line (must not be NULL).
 *   serverOut - writable FILE* to send responses/plays to the server.
 *   hand      - client's current Hand to read/modify based on plays.
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   May print to stdout, mutate the Hand (when a valid play is accepted),
 *   and write protocol lines to serverOut.
 *
 * Error handling:
 *   Lines that do not conform to the protocol are ignored or reported
 *   according to the assignment’s requirements.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
void handle_message(const char *message, FILE* serverOut, Hand* hand) {
    switch (message[0]) {
        case 'M':
            printf("Info: %s", message+1);
            break;
        case 'A':
            handle_accept(hand);
            break;
        case 'L':
            handle_lead(serverOut, hand);
            break;
        case 'H':
            if (hand->count > 0) {
                fprintf(stderr, "ratsclient: a protocol error occurred\n");
                exit(7);
            }
            parse_hand_message(message, hand);
            display_hand(hand);
            break;
        case 'P': {
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
            exit(0);
            break;

        default:
            fprintf(stderr, "ratsclient: a protocol error occurred\n");
            exit(7);
    }
}


int main(int argc, char *argv[]) {
    // 1. Parse and validate arguments
    validate_arguments(argc, argv);
    // Ensure prompts/lines appear immediately when stdout is piped by tests
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
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

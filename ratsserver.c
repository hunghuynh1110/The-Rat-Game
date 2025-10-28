// ratsserver.c — Function 1 only: die_usage()
// ratsserver.c — Function 2: parse_maxconns()

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>

#include <signal.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <time.h>
#include "/local/courses/csse2310/include/csse2310a4.h"
#include <stdatomic.h>
#include <poll.h>

typedef struct ServerContext ServerContext; // forward-declare for pointer usage
typedef struct {
    int fd;
    const char *greeting;
    ServerContext *serverCtx;  // server state (no globals per spec)
} ClientArg;

#define MAX_GAME_NAME 256
#define MAX_PLAYERS 4
#define EXIT_INVALID_PORT 1  // ← set this to the spec’s † value if different

#define MAX_MSG_SIZE 64
#define HALF_MSG_SIZE 32
#define MAX_BUFFER_SIZE 256

#define MAX_ARGC 3
#define MAX_ARGC4 4

#define MAX_TRICK 13
#define MAX_SLEEP_TIME 100

#define MAX_TEAM_MSG 512

// AI-generated comment: constants to avoid magic numbers for rank mapping
#define RANKS_STRING        "23456789TJQKA"
#define RANK_LOWEST_VALUE   2
#define INVALID_RANK_VALUE  (-1)

#define MAX_LENGTH_ARG_STR 10000

#define NUM8 8
#define NUM26 26
#define MAX_HAND 52
#define NUM104 104

#define MAX_STR_LEN_10 10
#define MAX_CARD_RANK 26

#define LISTEN_PORT_ERROR 6
#define SYSTEM_ERROR 3
#define INVALID_ARG 16

typedef struct Game {
    char gameName[MAX_GAME_NAME];
    int playerCount;                    // number of players currently joined (0..4)
    int playerFds[MAX_PLAYERS];         // connected client fds by join order (we may reseat later)
    char* playerNames[MAX_PLAYERS];     // heap-allocated player names
    struct Game *next;                  // singly-linked list
} Game;

// All shared server state lives in this context and is passed around — no globals.
struct ServerContext {
    Game *pendingGamesHead;
    pthread_mutex_t pendingGamesMutex;

    unsigned maxConns;
    unsigned activeClients;
    pthread_cond_t canAccept;

    // Statistics
    atomic_uint totalPlayersConnected;
    atomic_uint gamesRunning;
    atomic_uint gamesCompleted;
    atomic_uint gamesTerminated;
    atomic_uint totalTricksPlayed;
    atomic_uint activeClientSockets;
};

// Server-side hand representation for each player (no globals; passed down)
typedef struct {
    char cards[MAX_CARD_RANK][2]; // [rank, suit]
    int count;         // remaining cards (start at 26)
} PlayerHand;



static void die_usage(void);
static bool parse_maxconns(const char* s, unsigned* out);
static int listen_and_report_port(const char* portMsg, const char* service);
static void block_sigpipe_all_threads(void);
static void *client_greeting_thread(void *threadArg);
static void accept_loop(int listenFd, const char *greeting, ServerContext *serverCtx);
static char *read_line_alloc(FILE *inStream);
static void send_line(FILE *outStream, const char *text);
static bool read_join_info(FILE *inStream, char **playerNameOut, char **gameNameOut);
static Game* get_or_create_pending_game(ServerContext* serverCtx, const char* gameName);
static int add_player_to_pending_game(ServerContext* serverCtx, Game* game, const char* playerName, int clientFd);
static int handle_client_join(ServerContext *serverCtx, int clientFd, 
    FILE *inStream, char **playerNameOut, Game **gameOut);
static void unlink_pending_game(ServerContext* serverCtx, Game* target);

static void acquire_conn_slot(ServerContext *serverCtx);
static void release_conn_slot(ServerContext *serverCtx);

static void start_game(ServerContext *serverCtx, Game *game);
static void broadcast_msg(FILE *outs[MAX_PLAYERS], const char *fmt, ...);
static void deal_and_send_hands(FILE *outs[MAX_PLAYERS], const char *deckStr);
static const char *get_deck_or_die(void);

static int rank_value(char rankChar);
static bool is_valid_rank(char rankChar);
static bool is_valid_suit(char suitChar);
static int winning_seat_in_trick(char leadSuit,  char plays[MAX_PLAYERS][2]);

static void build_hands_from_deck(const char *deckStr, PlayerHand hands[MAX_PLAYERS]);
static bool has_suit_in_hand(const PlayerHand *hand, char suitChar);
static bool remove_card_from_hand(PlayerHand *hand, char rankChar, char suitChar);


static bool parse_card_token(const char *line, char *rankOut, char *suitOut);
static int play_tricks(ServerContext *serverCtx, Game *game, FILE *ins[MAX_PLAYERS], FILE *outs[MAX_PLAYERS], PlayerHand hands[MAX_PLAYERS]);

static void announce_play(FILE *outs[MAX_PLAYERS], const Game* game, int seat, char rankChar, char suitChar);
static void announce_trick_winner(FILE *outs[MAX_PLAYERS], const Game* game, int winnerSeat);
static int seat_to_team(int seat);
static void announce_final_score(FILE *outs[MAX_PLAYERS], int team1Tricks, int team2Tricks);

//helper
static int read_and_apply_valid_card(ServerContext *serverCtx, Game *game,
                                     int seat, int trickOffset, bool isLeader,
                                     char *leadSuitInOut, FILE *in, FILE *out,
                                     PlayerHand *hand,
                                     FILE *outs[MAX_PLAYERS],
                                     char plays[MAX_PLAYERS][2]);
static int handle_disconnect_early(ServerContext *serverCtx, Game *game,
                                   int seat, FILE *outs[MAX_PLAYERS]);
static int play_single_trick(ServerContext *serverCtx, Game *game,
                             FILE *ins[MAX_PLAYERS], FILE *outs[MAX_PLAYERS],
                             PlayerHand hands[MAX_PLAYERS],
                             int leaderSeat, int *winnerSeatOut);

// SIGHUP
static void *stats_sigwait_thread(void *arg);
static void start_sighup_stats_thread(ServerContext *ctx);
static void *pending_fd_monitor_thread(void *arg);
static void start_pending_fd_monitor(ServerContext *ctx);

/**
 * die_usage
 * ---------
 * Prints the required usage message to stderr and terminates the process.
 * Intended to be called during argument validation when inputs are invalid.
 *
 * Parameters:
 *   (none)
 *
 * Returns:
 *   None (does not return; calls exit(16) per specification).
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void die_usage(void) {
    fprintf(stderr, "Usage: ./ratsserver maxconns greeting [portnum]\n");
    exit(INVALID_ARG);
}

/**
 * parse_maxconns
 * --------------
 * Validates and parses the maxconns command-line argument as an unsigned
 * decimal in the range [0, 10000]. Optional leading '+' is accepted.
 *
 * Parameters:
 *   s   - NUL-terminated input string to parse.
 *   out - On success, receives the parsed value.
 *
 * Returns:
 *   true  if parsing succeeds and *out is set.
 *   false otherwise (no writes to *out).
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static bool parse_maxconns(const char* s, unsigned* out) {
    if (!s || !*s) {
        return false;
    }

    if (isspace((unsigned char)s[0])) {
        return false;
    }
    if (s[0] == '-') {
        return false;
    }

    errno = 0;
    char* end = NULL;
    long v = strtol(s, &end, MAX_STR_LEN_10);  // accepts optional leading '+'
    if (errno != 0) {
        return false;
    }
    if (*end != '\0') { // no trailing junk allowed
        return false;
    }
    
    if (v < 0 || v > MAX_LENGTH_ARG_STR) {
        return false;
    }

    *out = (unsigned)v;
    return true;
}

/**
 * listen_and_report_port
 * ----------------------
 * Creates an IPv4 TCP listening socket for the given service/port and
 * prints the bound port number to stderr (newline-terminated).
 *
 * Parameters:
 *   portMsg - String echoed in the listen/bind error message (quoted).
 *   service - Service/port string passed to getaddrinfo() (e.g., "0", "12345").
 *
 * Returns:
 *   >= 0 on success: file descriptor of the listening socket.
 *   None on failure: does not return; exits with the spec-defined status
 *   for "port invalid" († value) or 6 for "unable to listen".
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static int listen_and_report_port(const char* portMsg, const char* service) {
    struct addrinfo hints, *res = NULL, *rp = NULL;
    memset(&hints, 0, sizeof hints);
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;//bind server side

    //Validate service/port string
    int gai = getaddrinfo(NULL, service, &hints, &res);//local => NULL
    if (gai!=0) {
        fprintf(stderr, "ratsserver: port invalid\n");
        exit(EXIT_INVALID_PORT);
    }

    // Bind + listen
    int lfd = -1;
    int yes = 1;
    for(rp = res; rp; rp = rp->ai_next) {
        //create socket, try next if fail
        lfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);

        if(lfd < 0) continue;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
        if(bind(lfd, rp->ai_addr, rp->ai_addrlen) == 0 && listen(lfd, SOMAXCONN) == 0) {
            break;
        }
        close(lfd);
        lfd = -1;
    }
    freeaddrinfo(res);

    // if none work
    if(lfd < 0) {
        fprintf(stderr, "ratsserver: unable to listen on given port \"%s\"\n", portMsg);
        exit(LISTEN_PORT_ERROR);
    }


    //print actual bound port
    struct sockaddr_in sin; socklen_t slen = sizeof sin;
    if(getsockname(lfd, (struct sockaddr*)&sin, &slen) == 0) {
        fprintf(stderr, "%u\n", (unsigned)ntohs(sin.sin_port));
        fflush(stderr);
    }
    return lfd;

}

/**
 * block_sigpipe_all_threads
 * -------------------------
 * Blocks SIGPIPE in the calling thread so write() to closed sockets does
 * not terminate the process. Should be called before creating any threads
 * so the mask is inherited.
 *
 * Parameters:
 *   (none)
 *
 * Returns:
 *   None.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void block_sigpipe_all_threads(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}


/**
 * client_greeting_thread
 * ----------------------
 * Thread entry point for a newly accepted client. Sends the greeting line,
 * reads player/game names, registers the client into a pending game, and
 * if the game reaches four players, unlinks it from the pending list and
 * starts the game. Cleans up the socket/slot on failure.
 *
 * Parameters:
 *   threadArg - Pointer to ClientArg { fd, greeting, serverCtx }.
 *
 * Returns:
 *   NULL (pthread start routine signature).
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void* client_greeting_thread(void* threadArg) {
    ClientArg* clientArg = (ClientArg*)threadArg;
    int clientFd = clientArg->fd;
    const char* greetingMessage = clientArg->greeting;
    ServerContext *serverCtx = clientArg->serverCtx;
    FILE* clientOut = fdopen(dup(clientFd), "w");
    if (clientOut) {
        fprintf(clientOut, "M%s\n", greetingMessage);
        fflush(clientOut);
        fclose(clientOut);
    }
    FILE* clientIn = fdopen(dup(clientFd), "r");
    if (!clientIn) {
        close(clientFd);
        atomic_fetch_sub(&serverCtx->activeClientSockets, 1u);
        release_conn_slot(serverCtx);
        free(clientArg);
        return NULL;
    }
    char *playerName = NULL;
    Game *game = NULL;
    int seatIndex = handle_client_join(serverCtx, clientFd, clientIn, &playerName, &game);
    fclose(clientIn);
    if (seatIndex < 0) {
        close(clientFd);
        atomic_fetch_sub(&serverCtx->activeClientSockets, 1u);
        release_conn_slot(serverCtx);
        free(playerName);
        free(clientArg);
        return NULL;
    }
    free(playerName);
    //check if full
    if (seatIndex < (MAX_PLAYERS - 1)) {
        free(clientArg);
        return NULL;
    }
    // seatIndex == 3 -> game just became full; prevent further joins on this game.
    unlink_pending_game(serverCtx, game);
    start_game(serverCtx, game);
    free(clientArg);
    return NULL;
}


/**
 * accept_loop
 * -----------
 * Main server accept loop. Respects the configured connection limit,
 * accepts incoming TCP connections, and spawns a detached
 * client_greeting_thread for each successfully accepted client.
 * Retries on EINTR and safely releases a reserved slot on other errors.
 *
 * Parameters:
 *   listenFd  - listening socket file descriptor (IPv4 TCP).
 *   greeting  - greeting message to send to each client (without "M" prefix).
 *   serverCtx - shared server state (connection limiting, pending games, stats).
 *
 * Returns:
 *   None (does not return under normal operation; serves indefinitely).
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void accept_loop(int listenFd, const char *greeting, ServerContext *serverCtx) {
    for (;;) {
        acquire_conn_slot(serverCtx);
        int clientFd = -1;
        bool restartOuter = false;
        for (;;) {
            struct sockaddr_in clientAddr;
            socklen_t clientLen = sizeof clientAddr;
            clientFd = accept(listenFd, (struct sockaddr *)&clientAddr, &clientLen);
            if (clientFd >= 0) break;
            if (errno == EINTR) continue;
            // Other errors: free slot and try the outer loop again
            release_conn_slot(serverCtx);
            restartOuter = true;
            break;
        }
        if (restartOuter) continue;
        atomic_fetch_add(&serverCtx->activeClientSockets, 1u);
        atomic_fetch_add(&serverCtx->totalPlayersConnected, 1u);
        ClientArg *clientArg = malloc(sizeof *clientArg);
        if (!clientArg) {
            close(clientFd);
            // undo the live-socket bump
            atomic_fetch_sub(&serverCtx->activeClientSockets, 1u);
            release_conn_slot(serverCtx);
            continue;
        }
        clientArg->fd = clientFd;
        clientArg->greeting = greeting;
        clientArg->serverCtx = serverCtx;
        pthread_t threadId;
        if (pthread_create(&threadId, NULL, client_greeting_thread, clientArg) != 0) {
            close(clientFd);
            // undo the live-socket bump
            atomic_fetch_sub(&serverCtx->activeClientSockets, 1u);
            release_conn_slot(serverCtx);
            free(clientArg);
            continue;
        }
        pthread_detach(threadId);
    }
}

/**
 * read_line_alloc
 * ---------------
 * Reads a single line from a text stream, allocating a buffer large enough
 * to store the entire line. Trailing '\n' and '\r' are stripped.
 *
 * Parameters:
 *   inStream - input FILE* opened for reading (must be non-NULL).
 *
 * Returns:
 *   Pointer to heap-allocated, NUL-terminated line (caller must free),
 *   or NULL on EOF or error (no allocation is retained).
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static char *read_line_alloc(FILE *inStream) {
    if(!inStream) {
        return NULL;
    }
    char *lineBuffer = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&lineBuffer, &capacity, inStream);
    if(length<0) {
        free(lineBuffer);
        return NULL;
    }

    // strip trailing newline
    while(length > 0 && (lineBuffer[length-1] == '\n' || lineBuffer[length-1] == '\r')) {
        length--;
        lineBuffer[length] = '\0';
    }
    return lineBuffer;
}

/**
 * send_line
 * ---------
 * Writes a text line followed by '\n' to the given output stream and flushes
 * the stream. A no-op if either argument is NULL.
 *
 * Parameters:
 *   outStream - output FILE* opened for writing.
 *   text      - NUL-terminated string to write (without trailing newline).
 *
 * Returns:
 *   None.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void send_line(FILE *outStream, const char *text) {
    if(!outStream || !text) {
        return;
    }

    fputs(text, outStream);
    fputc('\n', outStream);
    fflush(outStream);
    
}

/**
 * read_join_info
 * --------------
 * Reads the two-line join protocol from a client: player name (line 1) and
 * game name (line 2). Empty strings are rejected. Newlines are removed.
 *
 * Parameters:
 *   inStream      - input stream for the client connection.
 *   playerNameOut - on success, set to malloc'd player name (caller frees).
 *   gameNameOut   - on success, set to malloc'd game name (caller frees).
 *
 * Returns:
 *   true  if both lines are read and valid; outputs are set.
 *   false on EOF, error, or invalid input; outputs are not retained.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static bool read_join_info(FILE *inStream, char **playerNameOut, char **gameNameOut) {
    if(!inStream || !playerNameOut || !gameNameOut) {
        return false;
    }
    char *playerName = read_line_alloc(inStream);
    if(!playerName || playerName[0] == '\0') {
        free(playerName);
        return false;
    }

    char* gameName = read_line_alloc(inStream);
    if (!gameName || gameName[0] == '\0') {
        free(playerName);
        free(gameName);
        return false;
    }

    *playerNameOut = playerName;
    *gameNameOut = gameName;

    return true;
}

/**
 * get_or_create_pending_game
 * --------------------------
 * Looks up a pending game by name in the server registry. If not found,
 * creates a new pending game, initialises its fields, and inserts it into
 * the pending list. Thread-safe with respect to the registry mutex.
 *
 * Parameters:
 *   serverCtx - shared server context containing the pending-games list.
 *   gameName  - NUL-terminated game name key to find or create.
 *
 * Returns:
 *   Pointer to the existing or newly created Game on success,
 *   or NULL on allocation failure or invalid arguments.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static Game* get_or_create_pending_game(ServerContext* serverCtx, const char* gameName) {
    if(!serverCtx || !gameName || !*gameName) {
        return NULL;
    }

    pthread_mutex_lock(&serverCtx->pendingGamesMutex);

    //search existing games
    Game *game = serverCtx->pendingGamesHead;
    while(game) {
        if(strcmp(game->gameName , gameName) == 0) {
            pthread_mutex_unlock(&serverCtx->pendingGamesMutex);
            return game;
        }
        game = game->next;
    }

    //not found
    Game *newGame = calloc(1, sizeof *newGame);
    if(!newGame) {
        pthread_mutex_unlock(&serverCtx->pendingGamesMutex);
        return NULL;
    }

    snprintf(newGame->gameName, sizeof newGame->gameName, "%s", gameName);
    newGame->playerCount = 0;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        newGame->playerFds[i] = -1;
        newGame->playerNames[i] = NULL;
    }
    newGame->next = serverCtx->pendingGamesHead;
    serverCtx->pendingGamesHead = newGame;

    pthread_mutex_unlock(&serverCtx->pendingGamesMutex);
    return newGame;
}

/**
 * add_player_to_pending_game
 * --------------------------
 * Registers a client socket and player name into a pending Game. Seats are
 * assigned by join order (0..3). Duplicates the player name for storage.
 * The pending-games registry is protected by a mutex inside this function.
 *
 * Parameters:
 *   serverCtx - shared server context (must be non-NULL).
 *   game      - pending game to join (must be non-NULL).
 *   playerName- NUL-terminated player name to store (must be non-NULL/non-empty).
 *   clientFd  - connected client socket file descriptor (>= 0).
 *
 * Returns:
 *   Seat index in the range [0, 3] on success.
 *   -1 on failure (invalid args, game already full, or allocation failure).
 *
 * Notes:
 *   - This function does not modify connection counters or totalPlayersConnected;
 *     those are handled at accept time and game teardown.
 *   - On allocation failure, the game is left unchanged.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static int add_player_to_pending_game(ServerContext* serverCtx, Game* game, const char* playerName, int clientFd) {
    if (!serverCtx || !game || !playerName || !*playerName || clientFd < 0) {
        return -1;
    }

    pthread_mutex_lock(&serverCtx->pendingGamesMutex);

    if (game->playerCount >= MAX_PLAYERS) {
        pthread_mutex_unlock(&serverCtx->pendingGamesMutex);
        return -1;
    }

    int seatIndex = game->playerCount;      // join order; seating may be rearranged later
    game->playerFds[seatIndex] = clientFd;
    game->playerNames[seatIndex] = strdup(playerName);
    if (!game->playerNames[seatIndex]) {
        game->playerFds[seatIndex] = -1;
        pthread_mutex_unlock(&serverCtx->pendingGamesMutex);
        return -1;
    }

    game->playerCount++;
    // NOTE: Do NOT bump totalPlayersConnected here; it represents accepted sockets.

    pthread_mutex_unlock(&serverCtx->pendingGamesMutex);
    return seatIndex;
}

/**
 * handle_client_join
 * ------------------
 * Orchestrates the "join" phase for a connected client: reads the two-line
 * join payload (player name, game name), finds or creates the pending game,
 * and adds the player to that game.
 *
 * Parameters:
 *   serverCtx      - shared server context (must be non-NULL).
 *   clientFd       - connected client socket descriptor (>= 0).
 *   inStream       - FILE* for reading from the client (must be non-NULL).
 *   playerNameOut  - on success, set to malloc'd copy of the player's name
 *                    (caller must free).
 *   gameOut        - on success, set to the target Game* (owned by server).
 *
 * Returns:
 *   Seat index in the range [0, 3] on success.
 *   -1 on failure (protocol error, allocation failure, or full game).
 *
 * Notes:
 *   - On any failure, frees any temporary allocations and leaves outputs unset.
 *   - Does not close clientFd; caller remains responsible for the fd lifetime.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static int handle_client_join(ServerContext *serverCtx, int clientFd
        ,FILE *inStream, char **playerNameOut, Game **gameOut) {
        if(!serverCtx || clientFd<0 || !inStream || !playerNameOut || !gameOut) {
            return -1;
        }

        char* playerName = NULL;
        char *gameName = NULL;
        if(!read_join_info(inStream, &playerName, &gameName)) {
            // EOF / protocol error
            free(playerName);
            free(gameName);
            return -1;
        }

        Game *game = get_or_create_pending_game(serverCtx, gameName);
        if(!game) {
            free(playerName);
            free(gameName);
            return -1;
        }

        int seatIndex = add_player_to_pending_game(serverCtx, game, playerName, clientFd);
        if(seatIndex < 0) {
            free(playerName);
            free(gameName);
            return -1;
        }

        *playerNameOut = playerName;
        *gameOut = game;
        free(gameName);
        return seatIndex;
}

/**
 * unlink_pending_game
 * -------------------
 * Removes a pending Game from the server's pending-games singly linked list.
 * The function does not free the Game; it only detaches it from the list and
 * sets target->next to NULL. Thread-safe via the registry mutex.
 *
 * Parameters:
 *   serverCtx - shared server context (must be non-NULL).
 *   target    - the Game to remove from the pending list (must be non-NULL).
 *
 * Returns:
 *   None.
 *
 * Notes:
 *   - Safe to call even if the game is not present; in that case the list
 *     is left unchanged.
 *   - Typical usage is right before starting a full game (seat count == 4).
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void unlink_pending_game(ServerContext* serverCtx, Game* target) {
    if(!serverCtx || !target) {
        return;
    }

    pthread_mutex_lock(&serverCtx->pendingGamesMutex);
    Game **cursor = &serverCtx->pendingGamesHead;
    while(*cursor) {
        if(*cursor == target) {
            *cursor = target->next;
            target->next = NULL;
            break;
        }
        cursor = &(*cursor)->next;
    }
    pthread_mutex_unlock(&serverCtx->pendingGamesMutex);
}

/**
 * acquire_conn_slot
 * -----------------
 * Enforces the max connection limit. If maxConns > 0, blocks until the number
 * of active clients is below the limit, then reserves a slot by incrementing
 * the activeClients counter. Thread-safe via mutex and condition variable.
 *
 * Parameters:
 *   serverCtx - shared server context (must be non-NULL).
 *
 * Returns:
 *   None.
 *
 * Notes:
 *   - If maxConns == 0, there is effectively no limit and the call returns
 *     immediately after incrementing activeClients.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void acquire_conn_slot(ServerContext *serverCtx) {
    if (!serverCtx) {
        return;
    }
    pthread_mutex_lock(&serverCtx->pendingGamesMutex);
    if (serverCtx->maxConns > 0) {
        while (serverCtx->activeClients >= serverCtx->maxConns) {
            pthread_cond_wait(&serverCtx->canAccept, &serverCtx->pendingGamesMutex);
        }
    }
    serverCtx->activeClients++;
    pthread_mutex_unlock(&serverCtx->pendingGamesMutex);
}

/**
 * release_conn_slot
 * -----------------
 * Releases a previously acquired connection slot by decrementing the
 * activeClients counter (if non-zero) and signaling one waiter on the
 * acceptance condition variable. Thread-safe.
 *
 * Parameters:
 *   serverCtx - shared server context (must be non-NULL).
 *
 * Returns:
 *   None.
 *
 * Notes:
 *   - Defensive against underflow: if activeClients is already zero, the
 *     counter is left unchanged and a signal is still emitted to wake any
 *     potential waiters.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void release_conn_slot(ServerContext *serverCtx) {
    if (!serverCtx) {
        return;
    }
    pthread_mutex_lock(&serverCtx->pendingGamesMutex);
    if (serverCtx->activeClients > 0) {
        serverCtx->activeClients--;
    }
    pthread_cond_signal(&serverCtx->canAccept);
    pthread_mutex_unlock(&serverCtx->pendingGamesMutex);
}

/**
 * broadcast_msg
 * -------------
 * Convenience helper to printf-format a message once and send it to all
 * non-NULL output streams in the given array, flushing each.
 *
 * Parameters:
 *   outs - array of 4 FILE* (some may be NULL).
 *   fmt  - printf-style format string for the message.
 *   ...  - printf-style arguments corresponding to fmt.
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   Writes to each non-NULL FILE* and fflush()es it.
 *
 * Errors:
 *   Silent on individual stream write errors; moves on to the next stream.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void broadcast_msg(FILE *outs[MAX_PLAYERS], const char *fmt, ...) {
    if (!outs || !fmt) {
        return;
    }
    va_list ap0;
    va_start(ap0, fmt);
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (!outs[i]) continue;
        va_list ap;
        va_copy(ap, ap0);
        vfprintf(outs[i], fmt, ap);
        va_end(ap);
        fflush(outs[i]);
    }
    va_end(ap0);
}

/**
 * deal_and_send_hands
 * -------------------
 * Splits a 104-character deck string into four 26-card hands using the
 * assignment’s dealing pattern and sends each hand as an H-line to the
 * corresponding player output stream.
 *
 * Parameters:
 *   outs    - array of 4 FILE* (targets for H-lines; may contain NULLs).
 *   deckStr - pointer to 104-character deck string (rank/suit pairs).
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   Emits one line per player in the form "H<52 chars>\\n" and flushes it.
 *
 * Preconditions:
 *   deckStr must reference exactly 104 characters (52 cards × 2 chars).
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void deal_and_send_hands(FILE *outs[MAX_PLAYERS], const char *deckStr) {
    if(!deckStr) {
        return;
    }
    char hand[MAX_HAND + 1];
    hand[MAX_HAND] = '\0';
    char line[MAX_HAND+2];
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        int k = 0;
        for (int i = p * 2; i < NUM104 && k < NUM26; i+=NUM8) {
            hand[k++] = deckStr[i];
            hand[k++] = deckStr[i + 1];
        }

        if(outs[p]) {
            snprintf(line, sizeof line, "H%.*s", MAX_HAND, hand);
            fputs(line, outs[p]);
            fputc('\n', outs[p]);
            fflush(outs[p]);
        }
    }
}

/**
 * get_deck_or_die
 * ---------------
 * Retrieves a random deck string from the course-supplied library. Exits
 * with a system-error status if the library fails to provide a deck.
 *
 * Parameters:
 *   None.
 *
 * Returns:
 *   const char* to a 104-character deck string on success (never NULL).
 *
 * Errors:
 *   On failure, prints "ratsserver: system error" to stderr and exits(3).
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static const char *get_deck_or_die(void) {
    const char *deck = get_random_deck();
    if (!deck) {
        // Spec: system error path
        fprintf(stderr, "ratsserver: system error\n");
        exit(SYSTEM_ERROR);
    }
    return deck;
}

/**
 * rank_value
 * ----------
 * Maps rank chars in RANKS_STRING to numeric values starting at RANK_LOWEST_VALUE.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static int rank_value(char rankChar) {
    const char *p = strchr(RANKS_STRING, rankChar);
    if (!p) {
        return INVALID_RANK_VALUE;
    }
    return RANK_LOWEST_VALUE + (int)(p - RANKS_STRING);
}

/**
 * is_valid_rank
 * --------------
 * Checks whether a character is one of the permitted card ranks ('2'..'9',
 * 'T', 'J', 'Q', 'K', 'A').
 *
 * Parameters:
 *   rankChar - character to validate.
 *
 * Returns:
 *   true if rankChar is a valid rank; false otherwise.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static bool is_valid_rank(char rankChar) {
    return rank_value(rankChar) != INVALID_RANK_VALUE;
}

/**
 * is_valid_suit
 * --------------
 * Checks whether a character is one of the permitted suits ('S', 'C', 'D', 'H').
 *
 * Parameters:
 *   suitChar - character to validate.
 *
 * Returns:
 *   true if suitChar is a valid suit; false otherwise.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static bool is_valid_suit(char suitChar) {
    return suitChar == 'S' || suitChar == 'C' || suitChar == 'D' || suitChar == 'H';
}

/**
 * winning_seat_in_trick
 * ---------------------
 * Determines which of four plays won a trick, given the lead suit.
 * Only cards that match leadSuit can win; the highest rank among those wins.
 * If no card matches leadSuit (should not occur with proper validation),
 * seat 0 (the leader) is returned as a fallback.
 *
 * Parameters:
 *   leadSuit - suit that was led for this trick.
 *   plays    - 4×2 array of {rank, suit} plays in seat order 0..3.
 *
 * Returns:
 *   Seat index (0..3) that won the trick.
 *
 * Notes:
 *   Assumes plays[] entries are syntactically valid and present.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static int winning_seat_in_trick(char leadSuit,  char plays[MAX_PLAYERS][2]) {
    int winner = 0;
    int bestVal = -1;
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        char r = plays[i][0];
        char s = plays[i][1];
        if (s != leadSuit) {
            continue; // only lead-suit cards can win (no trumps defined in this game)
        }
        int v = rank_value(r);
        if (v > bestVal) {
            bestVal = v;
            winner = i;
        }
    }
    // If no card matched lead suit (shouldn't happen if validation enforces follow-suit),
    // fall back to treating seat 0 (the leader) as winner.
    if (bestVal < 0) {
        return 0;
    }
    return winner;
}


/**
 * build_hands_from_deck
 * ---------------------
 * Distributes a 104-character deck string into four 26-card hands using the
 * specified dealing pattern (every 8th character pair per player).
 *
 * Parameters:
 *   deckStr - pointer to 104-character deck string (rank/suit pairs).
 *   hands   - output array of 4 PlayerHand structures to populate.
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   Overwrites hands[p].cards and hands[p].count for all p in {0..3}.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void build_hands_from_deck(const char *deckStr, PlayerHand hands[MAX_PLAYERS]) {
    if(!deckStr || !hands) {
        return;
    }
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        hands[p].count = 0;
    }
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        for (int i = p * 2; i < NUM104 && hands[p].count < NUM26; i += NUM8) {
            int k = hands[p].count++;
            hands[p].cards[k][0] = deckStr[i];     // rank
            hands[p].cards[k][1] = deckStr[i + 1]; // suit
        }
    }
}

/**
 * has_suit_in_hand
 * ----------------
 * Tests whether a PlayerHand currently contains at least one card of suit suitChar.
 *
 * Parameters:
 *   hand     - pointer to PlayerHand to query.
 *   suitChar - suit to search for ('S', 'C', 'D', or 'H').
 *
 * Returns:
 *   true if any card in hand matches suitChar; false otherwise.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static bool has_suit_in_hand(const PlayerHand *hand, char suitChar) {
    if(!hand) {
        return false;
    }
    for (int i = 0; i < hand->count; ++i) {
        if(hand->cards[i][1] == suitChar) {
            return true;
        }
    }
    return false;
}

/**
 * remove_card_from_hand
 * ---------------------
 * Removes a specific (rank,suit) card from a PlayerHand if present. The
 * removal is O(n) and compacts the array by swapping the last element
 * into the removed position before decrementing count.
 *
 * Parameters:
 *   hand     - pointer to PlayerHand to modify (must not be NULL).
 *   rankChar - rank to remove ('2'..'9','T','J','Q','K','A').
 *   suitChar - suit to remove ('S','C','D','H').
 *
 * Returns:
 *   true if a matching card was found and removed; false otherwise.
 *
 * Side effects:
 *   Mutates the hand->cards array and hand->count.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static bool remove_card_from_hand(PlayerHand *hand, char rankChar, char suitChar) {
    if(!hand) {
        return false;
    }
    for (int i = 0; i < hand->count; ++i) {
        if(hand->cards[i][0] == rankChar && hand->cards[i][1] == suitChar) {
            int last = hand->count - 1;
            hand->cards[i][0] = hand->cards[last][0];
            hand->cards[i][1] = hand->cards[last][1];
            hand->count--;
            return true;
        }
    }

    return false;
}

/**
 * parse_card_token
 * ----------------
 * Parses a two-character card token into rank and suit and validates both.
 * A valid token has exact length 2 and uses permitted rank/suit symbols.
 *
 * Parameters:
 *   line     - NUL-terminated input token to parse (no trailing newline).
 *   rankOut  - output pointer for parsed rank (must not be NULL).
 *   suitOut  - output pointer for parsed suit (must not be NULL).
 *
 * Returns:
 *   true if line is exactly two characters and both rank and suit are valid;
 *   false otherwise. On failure, rankOut/suitOut are not written.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static bool parse_card_token(const char *line, char *rankOut, char *suitOut) {
    if(!line || !rankOut || !suitOut) {
        return false;
    }

    size_t len = strlen(line);
    if(len != 2) {
        return false;
    }

    char r = line[0];
    char s = line[1];

    if(!is_valid_rank(r) || !is_valid_suit(s)) {
        return false;
    }

    *rankOut = r;
    *suitOut = s;
    return true;
}

/**
 * play_tricks
 * -----------
 * Runs a full sequence of up to 13 tricks for the current game. For each trick,
 * the leader is prompted with "L" and followers with "P<leadSuit>". Inputs are
 * validated for format, card ownership, and follow-suit when possible. Each
 * accepted play is acknowledged with "A". The trick winner is broadcast via
 * announce_trick_winner() and becomes the next leader. On normal completion,
 * final scores are broadcast and each player receives "O".
 *
 * Parameters:
 *   serverCtx - pointer to shared ServerContext (stats updated during play).
 *   game      - current Game (used for player names in announcements).
 *   ins       - FILE* array for player inputs (index 0..3).
 *   outs      - FILE* array for player outputs (index 0..3).
 *   hands     - per-player hands; cards are removed as they are played.
 *
 * Returns:
 *   0 if the game completed normally;
 *   1 if the game terminated early due to a player disconnect (other players
 *     are informed with "M<name> disconnected early" and "O").
 *
 * Notes:
 *   - Increments totalTricksPlayed for each completed trick.
 *   - Increments gamesTerminated if a disconnect occurs mid-hand.
 *   - Uses announce_play() to inform other seats of each valid play.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static int play_tricks(ServerContext *serverCtx, Game *game,
                       FILE *ins[MAX_PLAYERS], FILE *outs[MAX_PLAYERS],
                       PlayerHand hands[MAX_PLAYERS]) {
    (void)game;
    int teamTricks[2] = {0, 0};
    int leaderSeat = 0;

    for (int trick = 0; trick < MAX_TRICK; ++trick) {
        int winnerSeat = 0;
        if (play_single_trick(serverCtx, game, ins, outs, hands,
                              leaderSeat, &winnerSeat)) {
            return 1; // terminated
        }
        teamTricks[seat_to_team(winnerSeat)]++;
        leaderSeat = winnerSeat;
    }

    announce_final_score(outs, teamTricks[0], teamTricks[1]);

    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (outs[i]) {
            fputs("O\n", outs[i]);
            fflush(outs[i]);
        }
    }
    return 0; // completed normally
}

// --- helpers: prompts & invalids (≤ 20 LOC each) ---
static void send_lead_or_play_prompt(FILE* out, bool isLeader, char leadSuit) {
    if (isLeader) {
        send_line(out, "L");
    } else {
        char buf[MAX_PLAYERS-1] = { 'P', leadSuit, '\0' };
        send_line(out, buf);
    }
}

static void send_invalid_and_reprompt(FILE* out, bool isLeader, char leadSuit) {
    if (isLeader) {
        send_line(out, "L");
    } else {
        send_line(out, "I");
        char buf[MAX_PLAYERS-1] = { 'P', leadSuit, '\0' };
        send_line(out, buf);
    }
}

// --- helper: announce early disconnect & terminate game (≈ 30 LOC) ---
static int handle_disconnect_early(ServerContext* serverCtx, Game* game,
                                   int seat, FILE* outs[MAX_PLAYERS]) {
    const char* disp = NULL;
    char fallback[MAX_PLAYERS-1];
    if (game && seat >= 0 && seat < MAX_PLAYERS &&
        game->playerNames[seat] && game->playerNames[seat][0]) {
        disp = game->playerNames[seat];
    } else {
        fallback[0] = 'P';
        fallback[1] = (char)('1' + seat);
        fallback[2] = '\0';
        disp = fallback;
    }
    for (int j = 0; j < MAX_PLAYERS; ++j) {
        if (j == seat || !outs[j]) continue;
        fprintf(outs[j], "M%s disconnected early\n", disp);
        fputs("O\n", outs[j]);
        fflush(outs[j]);
    }
    atomic_fetch_add(&serverCtx->gamesTerminated, 1u);
    return 1; // terminated
}

// --- helper: read, validate, apply a single play (≈ 45 LOC) ---
static int read_and_apply_valid_card(ServerContext* serverCtx, Game* game,
                                     int seat, int trickOffset, bool isLeader,
                                     char* leadSuitInOut, FILE* in, FILE* out,
                                     PlayerHand* hand,
                                     FILE* outs[MAX_PLAYERS],
                                     char plays[MAX_PLAYERS][2]) {
    for (;;) {
        char* line = read_line_alloc(in);
        if (!line) {
            return handle_disconnect_early(serverCtx, game, seat, outs);
        }

        char r = 0, s = 0;
        bool ok = parse_card_token(line, &r, &s);
        free(line);
        if (!ok) {
            send_invalid_and_reprompt(out, isLeader, *leadSuitInOut);
            continue;
        }

        if (!isLeader && has_suit_in_hand(hand, *leadSuitInOut) && s != *leadSuitInOut) {
            send_invalid_and_reprompt(out, false, *leadSuitInOut);
            continue;
        }

        if (!remove_card_from_hand(hand, r, s)) {
            send_invalid_and_reprompt(out, isLeader, *leadSuitInOut);
            continue;
        }

        if (isLeader) *leadSuitInOut = s;
        plays[trickOffset][0] = r;
        plays[trickOffset][1] = s;

        send_line(out, "A");
        announce_play(outs, game, seat, r, s);
        return 0; // success
    }
}

// --- helper: play a single trick; returns 0 ok / 1 terminated (≈ 45 LOC) ---
static int play_single_trick(ServerContext* serverCtx, Game* game,
                             FILE* ins[MAX_PLAYERS], FILE* outs[MAX_PLAYERS],
                             PlayerHand hands[MAX_PLAYERS],
                             int leaderSeat, int* winnerSeatOut) {
    char plays[MAX_PLAYERS][2] = {{0}};
    char leadSuit = 0;

    for (int offset = 0; offset < MAX_PLAYERS; ++offset) {
        int seat = (leaderSeat + offset) % MAX_PLAYERS;
        bool isLeader = (offset == 0);

        send_lead_or_play_prompt(outs[seat], isLeader, leadSuit);

        if (read_and_apply_valid_card(serverCtx, game, seat, offset, isLeader,
                                      &leadSuit, ins[seat], outs[seat], &hands[seat],
                                      outs, plays)) {
            return 1; // terminated
        }
    }

    int winOffset = winning_seat_in_trick(leadSuit, plays);
    int winnerSeat = (leaderSeat + winOffset) % MAX_PLAYERS;
    announce_trick_winner(outs, game, winnerSeat);
    atomic_fetch_add(&serverCtx->totalTricksPlayed, 1u);
    *winnerSeatOut = winnerSeat;
    return 0;
}

/**
 * announce_play
 * -------------
 * Broadcasts a human-readable play message to all players except the one who
 * played. Uses the player’s joined name when available; otherwise falls back
 * to the seat label "P1".."P4".
 *
 * Parameters:
 *   outs      - FILE* array for player outputs (index 0..3).
 *   game      - current Game with optional playerNames for display.
 *   seat      - seat index (0..3) of the player who played the card.
 *   rankChar  - rank of the card that was played.
 *   suitChar  - suit of the card that was played.
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   Writes lines of the form "M<name|Px> plays <rank><suit>\n" to outs[i] for
 *   all i != seat; flushes each stream.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void announce_play(FILE *outs[MAX_PLAYERS], const Game* game, int seat, char rankChar, char suitChar) {
    if (!outs) {
        return;
    }
    const char *name = NULL;
    char fallback[MAX_PLAYERS - 1] = "P?";
    if (game && seat >= 0 && seat < MAX_PLAYERS &&
        game->playerNames[seat] && game->playerNames[seat][0]) {
        name = game->playerNames[seat];
    } else {
        fallback[0] = 'P';
        fallback[1] = (char)('1' + (seat >= 0 && seat < MAX_PLAYERS ? seat : 0));
        fallback[2] = '\0';
    }
    const char *disp = name ? name : fallback;

    char msg[MAX_MSG_SIZE];
    snprintf(msg, sizeof msg, "M%s plays %c%c\n", disp, rankChar, suitChar);
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (i == seat) {
            continue; // do not echo the announcement back to the player who played
        }
        if (outs[i]) {
            fputs(msg, outs[i]);
            fflush(outs[i]);
        }
    }
}

/**
 * announce_trick_winner
 * ---------------------
 * Broadcasts the winner of the most recent trick to all players, using the
 * seat label "P1".."P4" (not the player’s name).
 *
 * Parameters:
 *   outs        - FILE* array for player outputs (index 0..3).
 *   game        - current Game (unused; present for symmetry).
 *   winnerSeat  - seat index (0..3) of the trick winner.
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   Writes "M<seatLabel> won\n" to all outs[i]; flushes each stream.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void announce_trick_winner(FILE *outs[MAX_PLAYERS], const Game* game, int winnerSeat) {
    (void)game; // winners are always announced as Px (seat labels), not by name
    if (!outs) {
        return;
    }
    char label[MAX_PLAYERS - 1] = {'P', (char)('1' + (winnerSeat >= 0 && winnerSeat < MAX_PLAYERS ? winnerSeat : 0)), '\0'};
    char msg[HALF_MSG_SIZE];
    snprintf(msg, sizeof msg, "M%s won\n", label);
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (outs[i]) {
            fputs(msg, outs[i]);
            fflush(outs[i]);
        }
    }
}

/**
 * seat_to_team
 * ------------
 * Maps a player seat index to its team number according to fixed seating:
 * seats 0 and 2 belong to Team 1 (index 0); seats 1 and 3 belong to Team 2
 * (index 1).
 *
 * Parameters:
 *   seat - zero-based seat index (expected range 0..3).
 *
 * Returns:
 *   0 for seats {0,2}; 1 for seats {1,3}. Behaviour is undefined for values
 *   outside 0..3 (callers must validate).
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static int seat_to_team(int seat) {
    return (seat % 2 == 0) ? 0 : 1;
}

/**
 * announce_final_score
 * --------------------
 * Broadcasts the game result to all players. If one team wins on trick
 * count, prints "MWinner is Team <1|2> (<n> tricks won)". If both teams
 * tie, prints "MGame result: Draw".
 *
 * Parameters:
 *   outs         - FILE* array of length 4 for player outputs (may contain NULLs).
 *   team1Tricks  - total tricks taken by Team 1 (seats 0 and 2).
 *   team2Tricks  - total tricks taken by Team 2 (seats 1 and 3).
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   Writes the outcome line to each non-NULL stream in outs and flushes it.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void announce_final_score(FILE *outs[MAX_PLAYERS], int team1Tricks, int team2Tricks) {
    if (!outs) {
        return;
    }
    // Determine winner and winning trick count; if draw, report draw explicitly (fallback).
    char line[MAX_MSG_SIZE];
    if (team1Tricks > team2Tricks) {
        snprintf(line, sizeof line, "MWinner is Team 1 (%d tricks won)\n", team1Tricks);
    } else if (team2Tricks > team1Tricks) {
        snprintf(line, sizeof line, "MWinner is Team 2 (%d tricks won)\n", team2Tricks);
    } else {
        // Draw case (not covered in public tests, but keep a sensible message)
        snprintf(line, sizeof line, "MGame result: Draw\n");
    }
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (outs[i]) {
            fputs(line, outs[i]);
            fflush(outs[i]);
        }
    }
}

/**
 * stats_sigwait_thread
 * --------------------
 * Dedicated signal-wait thread that listens for SIGHUP and prints server
 * statistics to standard error upon receipt. The main thread blocks SIGHUP
 * in all threads before creating this one, so only this thread receives it.
 *
 * Parameters:
 *   arg - pointer to ServerContext containing atomics for all statistics.
 *
 * Returns:
 *   NULL (never returns in normal operation; loops indefinitely).
 *
 * Side effects:
 *   On each SIGHUP, formats and writes the current counters to STDERR using
 *   write(2). Output includes connected players, total players, games running,
 *   games completed, games terminated, and total tricks played.
 *
 * Concurrency:
 *   Reads atomic<uint> counters with atomic_load. No locks required.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void *stats_sigwait_thread(void *arg) {
    ServerContext *ctx = (ServerContext *)arg;
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGHUP);
    for (;;) {
        int sig;
        if (sigwait(&set, &sig) == 0 && sig == SIGHUP) {
            unsigned connectedNow = atomic_load(&ctx->activeClientSockets);

            unsigned tot    = atomic_load(&ctx->totalPlayersConnected);
            unsigned running= atomic_load(&ctx->gamesRunning);
            unsigned done   = atomic_load(&ctx->gamesCompleted);
            unsigned term   = atomic_load(&ctx->gamesTerminated);
            unsigned tricks = atomic_load(&ctx->totalTricksPlayed);

            char buf[MAX_GAME_NAME];
            int n = snprintf(buf, sizeof buf,
                "Connected players: %u\n"
                "Total num players connected: %u\n"
                "Num games running: %u\n"
                "Games completed: %u\n"
                "Games terminated: %u\n"
                "Total tricks played: %u\n",
                connectedNow, tot, running, done, term, tricks);
            if (n > 0) { (void)write(STDERR_FILENO, buf, (size_t)n); }
        }
    }
    return NULL;
}

/**
 * start_sighup_stats_thread
 * -------------------------
 * Installs the SIGHUP handling model by blocking SIGHUP in the calling
 * thread (and thus in subsequently created threads) and starting a detached
 * stats_sigwait_thread to perform sigwait() and print statistics.
 *
 * Parameters:
 *   ctx - pointer to ServerContext shared with the stats thread.
 *
 * Returns:
 *   None. (Thread creation failures are ignored per assignment scope.)
 *
 * Side effects:
 *   Alters the calling thread’s signal mask to block SIGHUP; spawns and
 *   detaches a new pthread that waits for SIGHUP and reports stats.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void start_sighup_stats_thread(ServerContext *ctx) {
    // Block SIGHUP in all threads; the dedicated thread will sigwait()
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    pthread_t tid;
    (void)pthread_create(&tid, NULL, stats_sigwait_thread, ctx);
    (void)pthread_detach(tid);
}

/**
 * pending_fd_monitor_thread
 * -------------------------
 * Periodically scans all sockets in pending (not-yet-full) games to detect
 * disconnects (POLLHUP/POLLERR/POLLNVAL). When a pending player has dropped,
 * the function closes the fd, updates live/socket and connection-limit
 * counters, compacts the game’s arrays, and signals canAccept to wake an
 * acceptor blocked by maxconns.
 *
 * Parameters:
 *   arg - pointer to ServerContext (provides pending list, mutex, counters).
 *
 * Returns:
 *   NULL (runs forever under normal operation).
 *
 * Side effects:
 *   - Modifies pendingGames list entries under pendingGamesMutex.
 *   - Decrements activeClientSockets and activeClients when a pending fd dies.
 *   - Signals canAccept after freeing a connection slot.
 *   - Sleeps ~100ms per iteration via poll(NULL,0,100) (no prohibited sleeps).
 *
 * Concurrency:
 *   Acquires pendingGamesMutex while walking and editing the pending list.
 *   Uses non-blocking poll to avoid stalling the server.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void *pending_fd_monitor_thread(void *arg) {
    ServerContext *ctx = (ServerContext *)arg;
    for (;;) {
        pthread_mutex_lock(&ctx->pendingGamesMutex);
        for (Game *g = ctx->pendingGamesHead; g; g = g->next) {
            int pc = g->playerCount;
            for (int s = 0; s < pc; ++s) {
                int fd = g->playerFds[s];
                if (fd < 0) continue;
                struct pollfd pfd = { .fd = fd, .events = 0, .revents = 0 };
                int r = poll(&pfd, 1, 0);
                if (r > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) {
                    // Remove this pending player and compact arrays
                    close(fd);
                    atomic_fetch_sub(&ctx->activeClientSockets, 1u);   // NEW: keep live count correct
                    if (g->playerNames[s]) { free(g->playerNames[s]); g->playerNames[s] = NULL; }
                    for (int t = s + 1; t < pc; ++t) {
                        g->playerFds[t - 1]   = g->playerFds[t];
                        g->playerNames[t - 1] = g->playerNames[t];
                    }
                    g->playerFds[pc - 1] = -1;
                    g->playerNames[pc - 1] = NULL;
                    g->playerCount--;
                    if (ctx->activeClients > 0) { ctx->activeClients--; }
                    pthread_cond_signal(&ctx->canAccept);
                    pc = g->playerCount;
                    s--; // re-check current index after compaction
                }
            }
        }
        pthread_mutex_unlock(&ctx->pendingGamesMutex);
        (void)poll(NULL, 0, MAX_SLEEP_TIME); // 100ms sleep via poll (no prohibited nanosleep/usleep)
    }
    return NULL;
}

/**
 * start_pending_fd_monitor
 * ------------------------
 * Spawns and detaches a background thread that periodically scans the
 * pending-games list for disconnected client sockets and cleans them up.
 *
 * Parameters:
 *   ctx - pointer to ServerContext shared with the monitor thread.
 *
 * Returns:
 *   None. (Thread creation failures are ignored per assignment scope.)
 *
 * Side effects:
 *   Creates a detached pthread running pending_fd_monitor_thread().
 *
 * Concurrency:
 *   The created thread will acquire pendingGamesMutex when inspecting and
 *   mutating the pending list; callers must not hold that mutex here.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void start_pending_fd_monitor(ServerContext *ctx) {
    pthread_t tid;
    (void)pthread_create(&tid, NULL, pending_fd_monitor_thread, ctx);
    (void)pthread_detach(tid);
}

/**
 * start_game
 * ----------
 * Finalises a pending game now at four players: re-seats by lexicographic
 * player name, announces teams, deals/sends hands, plays all tricks, and
 * then tears down connections and resources, updating statistics.
 *
 * Parameters:
 *   serverCtx - pointer to ServerContext (counters, limits, mutex/cond).
 *   game      - fully populated Game removed from the pending list.
 *
 * Returns:
 *   None.
 *
 * Side effects:
 *   - Opens per-player FILE* streams (r/w), writes protocol lines.
 *   - Increments gamesRunning during play and decrements afterward.
 *   - Increments gamesCompleted if the game finishes normally.
 *   - Closes client fds, frees player names, and frees the Game.
 *   - Releases four connection-limit slots via release_conn_slot().
 *
 * Concurrency:
 *   Assumes the game has been unlinked from the pending list. Uses only
 *   local file streams and atomics; no pendingGamesMutex held during play.
 *
 * REF: Comments created by AI, reviewed and modified for assignment compliance.
 */
static void start_game(ServerContext* serverCtx, Game* game) {
    // Reseat by alphabetical player name so seats 0..3 are lexicographic
    int order[MAX_PLAYERS] = {0, 1, 2, MAX_PLAYERS-1};
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        for (int j = i + 1; j < MAX_PLAYERS; ++j) {
            const char *ai = game->playerNames[ order[i] ];
            const char *aj = game->playerNames[ order[j] ];
            int cmp;
            if (ai && aj)       cmp = strcmp(ai, aj);
            else if (ai && !aj) cmp = -1;
            else if (!ai && aj) cmp = 1;
            else                cmp = 0;
            if (cmp > 0) { int t = order[i]; order[i] = order[j]; order[j] = t; }
        }
    }
    int newFds[MAX_PLAYERS]     = {-1, -1, -1, -1};
    char *newNames[MAX_PLAYERS] = {NULL, NULL, NULL, NULL};
    for (int s = 0; s < MAX_PLAYERS; ++s) {
        int idx = order[s];
        newFds[s]   = game->playerFds[idx];
        newNames[s] = game->playerNames[idx];
    }
    for (int s = 0; s < MAX_PLAYERS; ++s) {
        game->playerFds[s]   = newFds[s];
        game->playerNames[s] = newNames[s];
    }

    FILE *outs[MAX_PLAYERS] = {0};
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (game->playerFds[i] >= 0) {
            outs[i] = fdopen(dup(game->playerFds[i]), "w");
            if (outs[i]) setvbuf(outs[i], NULL, _IOLBF, 0);
        }
    }

    // Announce teams
    char team1Msg[MAX_TEAM_MSG], team2Msg[MAX_TEAM_MSG];
    snprintf(team1Msg, sizeof team1Msg, "MTeam 1: %s, %s\n",
             game->playerNames[0] ? game->playerNames[0] : "P1",
             game->playerNames[2] ? game->playerNames[2] : "P3");
    snprintf(team2Msg, sizeof team2Msg, "MTeam 2: %s, %s\n",
             game->playerNames[1] ? game->playerNames[1] : "P2",
             game->playerNames[MAX_PLAYERS - 1] ? game->playerNames[MAX_PLAYERS - 1] : "P4");
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (outs[i]) { fputs(team1Msg, outs[i]); fputs(team2Msg, outs[i]); fflush(outs[i]); }
    }

    const char *deckStr = get_deck_or_die();
    deal_and_send_hands(outs, deckStr);

    PlayerHand hands[MAX_PLAYERS];
    build_hands_from_deck(deckStr, hands);

    broadcast_msg(outs, "MStarting the game\n");

    FILE *ins[MAX_PLAYERS] = {0};
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (game->playerFds[i] >= 0) {
            ins[i] = fdopen(dup(game->playerFds[i]), "r");
        }
    }

    atomic_fetch_add(&serverCtx->gamesRunning, 1u);
    int ended = play_tricks(serverCtx, game, ins, outs, hands);
    atomic_fetch_sub(&serverCtx->gamesRunning, 1u);
    if (ended == 0) {
        atomic_fetch_add(&serverCtx->gamesCompleted, 1u);
    }

    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (ins[i])  fclose(ins[i]);
        if (outs[i]) fclose(outs[i]);
    }

    // Close original sockets and free names (decrement live count per socket)
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (game->playerFds[i] >= 0) {
            close(game->playerFds[i]);
            atomic_fetch_sub(&serverCtx->activeClientSockets, 1u); // NEW
            game->playerFds[i] = -1;
        }
        free(game->playerNames[i]);
        game->playerNames[i] = NULL;
    }

    // Free connection-limit slots for those 4 players
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        release_conn_slot(serverCtx);
    }
    free(game);
}


int main(int argc, char** argv) {
    // Usage checking
    if (argc != MAX_ARGC && argc != MAX_ARGC4) {
        die_usage();
    }
    unsigned maxconnsValue = 0;
    if (!parse_maxconns(argv[1], &maxconnsValue)) {
        die_usage();
    }
    const char* greeting = argv[2];
    if (!*greeting) {
        die_usage();
    }
    const char* portArg = (argc == MAX_ARGC4) ? argv[MAX_PLAYERS - 1] : "0";
    if (argc == MAX_PLAYERS && !*portArg) {
        die_usage();
    }

    // Block SIGPIPE so writes to closed sockets don't kill the process
    block_sigpipe_all_threads();

    // Bind/listen; prints bound port to stderr
    int listenFd = listen_and_report_port(portArg, portArg);

    // Shared server context
    ServerContext serverCtx;
    serverCtx.pendingGamesHead = NULL;
    pthread_mutex_init(&serverCtx.pendingGamesMutex, NULL);
    serverCtx.maxConns = maxconnsValue;
    serverCtx.activeClients = 0;
    pthread_cond_init(&serverCtx.canAccept, NULL);

    // Init stats
    atomic_init(&serverCtx.totalPlayersConnected, 0);
    atomic_init(&serverCtx.gamesRunning,        0);
    atomic_init(&serverCtx.gamesCompleted,      0);
    atomic_init(&serverCtx.gamesTerminated,     0);
    atomic_init(&serverCtx.totalTricksPlayed,   0);
    atomic_init(&serverCtx.activeClientSockets, 0);

    // Start SIGHUP stats thread + pending-FD monitor
    start_sighup_stats_thread(&serverCtx);
    start_pending_fd_monitor(&serverCtx);

    // Serve forever
    accept_loop(listenFd, greeting, &serverCtx);
    return 0;
}


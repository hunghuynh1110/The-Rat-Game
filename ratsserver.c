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
    char cards[26][2]; // [rank, suit]
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

static void start_game(ServerContext *serverCtx, Game *target);
static void broadcast_msg(FILE *outs[4], const char *fmt, ...);
static void deal_and_send_hands(FILE *outs[4], const char *deckStr);
static const char *get_deck_or_die(void);

static int rank_value(char rankChar);
static bool is_valid_rank(char rankChar);
static bool is_valid_suit(char suitChar);
static int winning_seat_in_trick(char leadSuit,  char plays[4][2]);

static void build_hands_from_deck(const char *deckStr, PlayerHand hands[4]);
static bool has_suit_in_hand(const PlayerHand *hand, char suitChar);
static bool remove_card_from_hand(PlayerHand *hand, char rankChar, char suitChar);


static bool parse_card_token(const char *line, char *rankOut, char *suitOut);
static int play_tricks(ServerContext *serverCtx, Game *game, FILE *ins[4], FILE *outs[4], PlayerHand hands[4]);

static void announce_play(FILE *outs[4], const Game* game, int seat, char rankChar, char suitChar);
static void announce_trick_winner(FILE *outs[4], const Game* game, int winnerSeat);
static int seat_to_team(int seat);
static void announce_final_score(FILE *outs[4], int team1Tricks, int team2Tricks);


//SIGHUP
static void *stats_sigwait_thread(void *arg);
static void start_sighup_stats_thread(ServerContext *ctx);
static void *pending_fd_monitor_thread(void *arg);
static void start_pending_fd_monitor(ServerContext *ctx);

static void die_usage(void) {
    fprintf(stderr, "Usage: ./ratsserver maxconns greeting [portnum]\n");
    exit(16);
}

/**
 * The maxconns argument is given but it is not a non-negative integer less than or equal to 10,000. A leading 
 * sign is permitted (optional). Numbers with leading zeroes will not be tested, i.e. may be accepted or rejected
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
    long v = strtol(s, &end, 10);  // accepts optional leading '+'
    if (errno != 0) {
        return false;
    }
    if (*end != '\0') { // no trailing junk allowed
        return false;
    }
    
    if (v < 0 || v > 10000) {
        return false;
    }

    *out = (unsigned)v;
    return true;
}

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
        exit(6);
    }


    //print actual bound port
    struct sockaddr_in sin; socklen_t slen = sizeof sin;
    if(getsockname(lfd, (struct sockaddr*)&sin, &slen) == 0) {
        fprintf(stderr, "%u\n", (unsigned)ntohs(sin.sin_port));
        fflush(stderr);
    }
    return lfd;

}

static void block_sigpipe_all_threads(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}


/**
 * Per new connection:
 *  - send M&lt;greeting&gt;,
 *  - read join info (player name + game name),
 *  - register into a pending game,
 *  - when the 4th player joins, start the game (connections remain open).
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
    // TODO: 
    start_game(serverCtx, game);
    free(clientArg);
    return NULL;
}


/* #9: accept_loop — no goto */
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

        // NEW: count this accepted socket as a connected player,
        // and bump the cumulative total-players metric.
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

static void send_line(FILE *outStream, const char *text) {
    if(!outStream || !text) {
        return;
    }

    fputs(text, outStream);
    fputc('\n', outStream);
    fflush(outStream);
    
}

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
 * looks up a pending game by name in serverCtx -> create it at head if not found
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
* Adds a player to a pending game under the registry lock.
* Returns the seat index (0..3) on success, or -1 on failure (full/OOM/bad args)
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

// Connection limit: reserve a slot or wait until one is available.
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

// Release a slot and wake one waiter (if any).
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

static void broadcast_msg(FILE *outs[4], const char *fmt, ...) {
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

static void deal_and_send_hands(FILE *outs[4], const char *deckStr) {
    if(!deckStr) {
        return;
    }
    char hand[53];
    hand[52] = '\0';
    char line[64];
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        int k = 0;
        for (int i = p * 2; i < 104 && k < 26; i+=8) {
            hand[k++] = deckStr[i];
            hand[k++] = deckStr[i + 1];
        }

        if(outs[p]) {
            snprintf(line, sizeof line, "H%.*s", 52, hand);
            fputs(line, outs[p]);
            fputc('\n', outs[p]);
            fflush(outs[p]);
        }
    }
}

// Obtain a 104-char random deck string from the course library or exit with a system error.
static const char *get_deck_or_die(void) {
    const char *deck = get_random_deck();
    if (!deck) {
        // Spec: system error path
        fprintf(stderr, "ratsserver: system error\n");
        exit(3);
    }
    return deck;
}

// Return a strength value for rank comparison (2..A). Higher is stronger.
static int rank_value(char rankChar) {
    switch (rankChar) {
        case '2': return 2;
        case '3': return 3;
        case '4': return 4;
        case '5': return 5;
        case '6': return 6;
        case '7': return 7;
        case '8': return 8;
        case '9': return 9;
        case 'T': return 10;
        case 'J': return 11;
        case 'Q': return 12;
        case 'K': return 13;
        case 'A': return 14;
        default:  return -1;
    }
}

// Validate a single rank character against the allowed set.
static bool is_valid_rank(char rankChar) {
    return rank_value(rankChar) != -1;
}

// Validate suit character against allowed suits S, C, D, H.
static bool is_valid_suit(char suitChar) {
    return suitChar == 'S' || suitChar == 'C' || suitChar == 'D' || suitChar == 'H';
}

// Given a 4-play trick (each play is [rank, suit]) and the lead suit,
// return the seat index (0..3) that won the trick. Assumes plays are valid and present.
static int winning_seat_in_trick(char leadSuit,  char plays[4][2]) {
    int winner = 0;
    int bestVal = -1;
    for (int i = 0; i < 4; ++i) {
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

static void build_hands_from_deck(const char *deckStr, PlayerHand hands[4]) {
    if(!deckStr || !hands) {
        return;
    }
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        hands[p].count = 0;
    }
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        for (int i = p * 2; i < 104 && hands[p].count < 26; i += 8) {
            int k = hands[p].count++;
            hands[p].cards[k][0] = deckStr[i];     // rank
            hands[p].cards[k][1] = deckStr[i + 1]; // suit
        }
    }
}

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

// Return canonical seat labels P1..P4 for messaging fallbacks.

// NOTE: Announcements are disabled for submission (spec-only protocol). Kept as no-ops.
static void announce_play(FILE *outs[4], const Game* game, int seat, char rankChar, char suitChar) {
    if (!outs) {
        return;
    }
    const char *name = NULL;
    char fallback[3] = "P?";
    if (game && seat >= 0 && seat < MAX_PLAYERS &&
        game->playerNames[seat] && game->playerNames[seat][0]) {
        name = game->playerNames[seat];
    } else {
        fallback[0] = 'P';
        fallback[1] = (char)('1' + (seat >= 0 && seat < MAX_PLAYERS ? seat : 0));
        fallback[2] = '\0';
    }
    const char *disp = name ? name : fallback;

    char msg[64];
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

static void announce_trick_winner(FILE *outs[4], const Game* game, int winnerSeat) {
    (void)game; // winners are always announced as Px (seat labels), not by name
    if (!outs) {
        return;
    }
    char label[3] = {'P', (char)('1' + (winnerSeat >= 0 && winnerSeat < MAX_PLAYERS ? winnerSeat : 0)), '\0'};
    char msg[32];
    snprintf(msg, sizeof msg, "M%s won\n", label);
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (outs[i]) {
            fputs(msg, outs[i]);
            fflush(outs[i]);
        }
    }
}

// Map a seat index to a team: seats 0 & 2 -> team 0, seats 1 & 3 -> team 1
static int seat_to_team(int seat) {
    return (seat % 2 == 0) ? 0 : 1;
}

// Broadcast final trick counts and overall winner at end of game.
// Uses the "M..." channel consistent with other informational messages.
static void announce_final_score(FILE *outs[4], int team1Tricks, int team2Tricks) {
    if (!outs) {
        return;
    }
    // Determine winner and winning trick count; if draw, report draw explicitly (fallback).
    char line[64];
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

/* #7: play_tricks — return 0 if game completed, 1 if terminated early */
static int play_tricks(ServerContext *serverCtx, Game *game, FILE *ins[4], FILE *outs[4], PlayerHand hands[4]) {
    (void)game;

    int teamTricks[2] = {0, 0};
    int leaderSeat = 0;

    for (int trick = 0; trick < 13; ++trick) {
        char plays[4][2] = {{0}};
        char leadSuit = 0;

        for (int offset = 0; offset < MAX_PLAYERS; ++offset) {
            int seat = (leaderSeat + offset) % MAX_PLAYERS;

            if (offset == 0) {
                send_line(outs[seat], "L");
            } else {
                char prompt[3] = { 'P', leadSuit, '\0' };
                send_line(outs[seat], prompt);
            }

            for (;;) {
                char *line = read_line_alloc(ins[seat]);
                if (!line) {
                    // Seat disconnected: tell others and end the game as terminated.
                    char label[3] = { 'P', (char)('1' + seat), '\0' };
                    for (int j = 0; j < MAX_PLAYERS; ++j) {
                        if (j == seat || !outs[j]) continue;
                        fprintf(outs[j], "M%s disconnected early\n", label);
                        fputs("O\n", outs[j]);
                        fflush(outs[j]);
                    }
                    atomic_fetch_add(&serverCtx->gamesTerminated, 1u);
                    return 1; // terminated
                }

                char r, s;
                bool ok = parse_card_token(line, &r, &s);
                free(line);

                if (!ok) {
                    if (offset == 0) {
                        send_line(outs[seat], "L");
                    } else {
                        send_line(outs[seat], "I");
                        char rePrompt[3] = { 'P', leadSuit, '\0' };
                        send_line(outs[seat], rePrompt);
                    }
                    continue;
                }

                // Must follow suit if possible (for followers)
                if (offset > 0 && has_suit_in_hand(&hands[seat], leadSuit) && s != leadSuit) {
                    send_line(outs[seat], "I");
                    char rePrompt[3] = { 'P', leadSuit, '\0' };
                    send_line(outs[seat], rePrompt);
                    continue;
                }

                // Card must exist in player's hand
                if (!remove_card_from_hand(&hands[seat], r, s)) {
                    if (offset == 0) {
                        send_line(outs[seat], "L");
                    } else {
                        send_line(outs[seat], "I");
                        char rePrompt[3] = { 'P', leadSuit, '\0' };
                        send_line(outs[seat], rePrompt);
                    }
                    continue;
                }

                plays[offset][0] = r;
                plays[offset][1] = s;
                if (offset == 0) {
                    leadSuit = s;
                }
                send_line(outs[seat], "A");
                announce_play(outs, game, seat, r, s);
                break;
            }
        }

        int winOffset = winning_seat_in_trick(leadSuit, plays);
        int winnerSeat = (leaderSeat + winOffset) % MAX_PLAYERS;
        announce_trick_winner(outs, game, winnerSeat);
        teamTricks[seat_to_team(winnerSeat)]++;
        atomic_fetch_add(&serverCtx->totalTricksPlayed, 1u);
        leaderSeat = winnerSeat;
    }

    announce_final_score(outs, teamTricks[0], teamTricks[1]);

    // Game over: send O to all
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (outs[i]) {
            fputs("O\n", outs[i]);
            fflush(outs[i]);
        }
    }
    return 0; // completed normally
}


/* #8: start_game — bump gamesRunning/completed around play_tricks */
static void start_game(ServerContext* serverCtx, Game* game) {
    // Reseat by alphabetical player name so seats 0..3 are lexicographic
    int order[4] = {0, 1, 2, 3};
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
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
    int newFds[4]     = {-1, -1, -1, -1};
    char *newNames[4] = {NULL, NULL, NULL, NULL};
    for (int s = 0; s < 4; ++s) {
        int idx = order[s];
        newFds[s]   = game->playerFds[idx];
        newNames[s] = game->playerNames[idx];
    }
    for (int s = 0; s < 4; ++s) {
        game->playerFds[s]   = newFds[s];
        game->playerNames[s] = newNames[s];
    }

    FILE *outs[4] = {0};
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (game->playerFds[i] >= 0) {
            outs[i] = fdopen(dup(game->playerFds[i]), "w");
            if (outs[i]) setvbuf(outs[i], NULL, _IOLBF, 0);
        }
    }

    // Announce teams
    char team1Msg[512], team2Msg[512];
    snprintf(team1Msg, sizeof team1Msg, "MTeam 1: %s, %s\n",
             game->playerNames[0] ? game->playerNames[0] : "P1",
             game->playerNames[2] ? game->playerNames[2] : "P3");
    snprintf(team2Msg, sizeof team2Msg, "MTeam 2: %s, %s\n",
             game->playerNames[1] ? game->playerNames[1] : "P2",
             game->playerNames[3] ? game->playerNames[3] : "P4");
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (outs[i]) { fputs(team1Msg, outs[i]); fputs(team2Msg, outs[i]); fflush(outs[i]); }
    }

    const char *deckStr = get_deck_or_die();
    deal_and_send_hands(outs, deckStr);

    PlayerHand hands[4];
    build_hands_from_deck(deckStr, hands);

    broadcast_msg(outs, "MStarting the game\n");

    FILE *ins[4] = {0};
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

            char buf[256];
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

// Monitor pending-game sockets so disconnects free slots & don’t skew stats
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
        (void)poll(NULL, 0, 100); // 100ms sleep via poll (no prohibited nanosleep/usleep)
    }
    return NULL;
}

static void start_pending_fd_monitor(ServerContext *ctx) {
    pthread_t tid;
    (void)pthread_create(&tid, NULL, pending_fd_monitor_thread, ctx);
    (void)pthread_detach(tid);
}

/* #10: main — init atomics and start SIGHUP/pending monitors before serving */
int main(int argc, char** argv) {
    // Usage checking
    if (argc != 3 && argc != 4) {
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
    const char* portArg = (argc == 4) ? argv[3] : "0";
    if (argc == 4 && !*portArg) {
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


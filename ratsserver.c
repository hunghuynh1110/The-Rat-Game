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
};


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

static void start_game(ServerContext *serverCtx, Game *target);
static void broadcast_msg(FILE *outs[4], const char *fmt, ...);
static void deal_and_send_hands(FILE *outs[4], const char *deckStr);



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

    // Disallow leading whitespace; allow optional leading '+'
    if (isspace((unsigned char)s[0])) {
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

static void block_sigpipe_all_threads(void) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGPIPE);

    pthread_sigmask(SIG_BLOCK, &set, NULL);
}

// per new connection, immediately send M<greeting> then close.
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
        free(clientArg);
        return NULL;
    }

    char *playerName = NULL;
    Game *game = NULL;

    int seatIndex = handle_client_join(serverCtx, clientFd, clientIn, &playerName, &game);
    fclose(clientIn);

    if(seatIndex<0) {
        close(clientFd);
        free(playerName);
        free(clientArg);
        return NULL;
    }

    free(playerName);

    //check if full
    if(seatIndex < (MAX_PLAYERS-1)) {
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


static void accept_loop(int listenFd, const char *greeting, ServerContext *serverCtx) {
    while(true) {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof clientAddr;

        int clientFd = accept(listenFd, (struct sockaddr *)&clientAddr, &clientLen);
        if(clientFd < 0) {
            if(errno == EINTR) {
                continue; // signal interrupt
            }
            //non fatal error
            continue;
        }

        ClientArg *clientArg = malloc(sizeof *clientArg);
        if (!clientArg) {
            close(clientFd);
            continue;
        }
        clientArg->fd = clientFd;
        clientArg->greeting = greeting;
        clientArg->serverCtx = serverCtx;

        pthread_t threadId;
        if(pthread_create(&threadId, NULL, client_greeting_thread, clientArg) != 0) {
            close(clientFd);
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

//looks up a pending game by name in serverCtx -> create it at head if not found
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

// Adds a player to a pending game under the registry lock.
// Returns the seat index (0..3) on success, or -1 on failure (full/OOM/bad args).
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

static void broadcast_msg(FILE *outs[4], const char *fmt, ...) {
    if (!outs || !fmt) {
        return;
    }
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if (!outs[i]) {
            continue;
        }
        va_list ap;
        va_start(ap, fmt);
        vfprintf(outs[i], fmt, ap);
        va_end(ap);
        fflush(outs[i]);
    }
}

static void deal_and_send_hands(FILE *outs[4], const char *deckStr) {
    if(!deckStr) {
        return;
    }
    char hand[27];
    hand[26] = '\0';
    char line[32];
    for (int p = 0; p < MAX_PLAYERS; ++p) {
        int k = 0;
        for (int i = p * 2; i < 104 && k < 26; i+=8) {
            hand[k++] = deckStr[i];
            hand[k++] = deckStr[i + 1];
        }

        if(outs[p]) {
            snprintf(line, sizeof line, "H%.*s", 26, hand);
            fputs(line, outs[p]);
            fputc('\n', outs[p]);
            fflush(outs[p]);
        }
    }
}

static void start_game(ServerContext* serverCtx, Game* game) {
    (void)serverCtx;

    FILE *outs[4] = {0};
    for (int i = 0; i < MAX_PLAYERS; ++i) {
        if(game->playerFds[i] >= 0) {
            outs[i] = fdopen(dup(game->playerFds[i]), "w");
        }
    }

    //announce team
    char team1Msg[512], team2Msg[512];
    snprintf(team1Msg, sizeof team1Msg, "MTeam 1: %s, %s\n",
             game->playerNames[0] ? game->playerNames[0] : "P1",
             game->playerNames[2] ? game->playerNames[2] : "P3");
    snprintf(team2Msg, sizeof team2Msg, "MTeam 2: %s, %s\n",
             game->playerNames[1] ? game->playerNames[1] : "P2",
             game->playerNames[3] ? game->playerNames[3] : "P4");

    for (int i = 0; i < MAX_PLAYERS;++i) {
        if(outs[i]) {
            fputs(team1Msg, outs[i]);
            fputs(team2Msg, outs[i]);
            fflush(outs[i]);
        }
    }

    const char *deckStr = get_random_deck();
    deal_and_send_hands(outs, deckStr);


    //announce start
    broadcast_msg(outs, "MStarting the game\n");

    //close write streams
    for (int i = 0; i < MAX_PLAYERS;++i) {
        if(outs[i]) {
            fclose(outs[i]);
        }
    }
}

int main(int argc, char** argv) {
    // Usage checking: only shape/emptiness here
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

    // Validate/bind/listen on the port; prints actual bound port to stderr
    int listenFd = listen_and_report_port(portArg, portArg);

    // Initialize shared server context (no globals)
    ServerContext serverCtx;
    serverCtx.pendingGamesHead = NULL;
    pthread_mutex_init(&serverCtx.pendingGamesMutex, NULL);
    serverCtx.maxConns = maxconnsValue;
    serverCtx.activeClients = 0;
    pthread_cond_init(&serverCtx.canAccept, NULL);

    // Serve forever
    accept_loop(listenFd, greeting, &serverCtx);
    return 0;
}

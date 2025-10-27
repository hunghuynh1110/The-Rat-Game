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

#define EXIT_INVALID_PORT 1  // ← set this to the spec’s † value if different

typedef struct {
    int fd;
    const char *greeting;
} ClientArg;


static void die_usage(void);
static bool parse_maxconns(const char* s, unsigned* out);
static int listen_and_report_port(const char* portMsg, const char* service);
static void block_sigpipe_all_threads(void);
static void accept_loop(int listenFd, const char *greeting);
static char *read_line_alloc(FILE *inStream);
static void send_line(FILE *outStream, const char *text);

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

    // if none workk
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

    FILE* clientOut = fdopen(dup(clientFd), "w");
    if (clientOut) {
        fprintf(clientOut, "M%s\n", greetingMessage);
        fflush(clientOut);
        fclose(clientOut);
    }

    close(clientFd);
    free(clientArg);
    return NULL;
}


// main server loop that accepts clients and hands each to a detached thread that sends the greeting.
static void accept_loop(int listenFd, const char *greeting) {
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
        if(!clientArg) {
            close(clientFd);
            continue;
        }
        clientArg->fd = clientFd;
        clientArg->greeting = greeting;

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

    // TODO (later): enforce maxconnsValue with a mutex/cond around accept()
    (void)maxconnsValue;

    // Serve forever
    accept_loop(listenFd, greeting);
    return 0;
}

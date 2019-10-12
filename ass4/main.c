#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>

#include <unistd.h>
#include <sys/types.h>

#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>


#include "exitCodes.h"
#include "connection.h"
#include "material.h"
#include "depotState.h"
#include "messages.h"
#include "util.h"

#define BANNED_NAME_CHARS " \n\r:"
#define CONNECTION_QUEUE 32

typedef struct ServerData {
    int fd; // file descriptor of the server
    int ourPort; // this depot's port
    char* ourName; // BORROWED this depot's name
    Channel* incoming; // BORROWED incoming message channel
} ServerData;

typedef struct VerifyData {
    int ourPort;
    char* ourName; // BORROW
    Channel* incoming;

    FILE* readFile; // OWNED
    FILE* writeFile; // OWNED
} VerifyData;

typedef struct ConnectorData {
    Connection* connection; // BORROWED reference to this connection
    Channel* incoming; // BORROWED reference to depot's incoming msg channel
} ConnectorData;

typedef struct WriterData {
    FILE* writeFile; // BORROWED file object to write encoded messages to
    Channel* outgoing; // BORROWED channel to get messages to write from
} WriterData;

typedef struct ReaderData {
    int port; // port of this connection
    char* name; // BORROW name of this connection's depot
    FILE* readFile; // BORROWED file object to read and decode messages from
    Channel* incoming; // BORROWED channel to write parsed messages to
} ReaderData;

// see impl. thread to verify connection with IM messages
void* verify_thread(void* verifyArg);
// see impl. thread to start and manage read/write threads
void* connector_thread(void* connectorDataArg);
// see impl. thread to write to socket
void* writer_thread(void* writerDataArg);
// see impl. thread to read from socket
void* reader_thread(void* readerDataArg);

// see implementations for comments.
void start_connector_thread(Connection* connection, Channel* incoming);
// see implementation
void start_verify_thread(int port, char* name, Channel* incoming, int fd);
// see implementation.
bool start_active_socket(int* fdOut, char* port);
// see implementaiton
void execute_message(DepotState* depotState, Message* message);


/* Returns whether the given string is a valid depot or material name,
 * according to rules in spec.
 */
bool is_name_valid(char* name) {
    int len = strlen(name);
    char* banned = BANNED_NAME_CHARS;
    for (int i = 0; i < len; i++) {
        // check if any char in name appears in banned list.
        if (strchr(banned, name[i]) != NULL) {
            DEBUG_PRINTF("name invalid: |%s|\n", name);
            return false;
        }
    }
    return len > 0; // ensure name non-empty
}

/* Returns whether the givne material is a valid message argument. 
 * That is, its name is valid and its quantity is strictly positive.
 */
bool is_mat_valid(Material material) {
    if (!is_name_valid(material.name)) {
        DEBUG_PRINT("invalid mat name");
        return false;
    }
    if (material.quantity <= 0) {
        DEBUG_PRINT("invalid mat quantity");
        return false;
    }
    return true;
}


void execute_connect(DepotState* depotState, Message* message) {
    char* port = asprintf("%d", message->data.depotPort);
    DEBUG_PRINTF("trying to connect to port %s\n", port);
    int fd;
    bool started = start_active_socket(&fd, port);
    free(port);
    if (started) {
        DEBUG_PRINT("connection established, verifying");
        start_verify_thread(depotState->port, depotState->name, 
                depotState->incoming, fd);
    } else {
        DEBUG_PRINT("failed to connect");
    }
}

void execute_deliver_withdraw(DepotState* depotState, Message* message) {
    if (!is_mat_valid(message->data.material)) {
        DEBUG_PRINT("ignoring invalid material");
        return;
    }

    int delta = message->data.material.quantity;
    if (message->type != MSG_DELIVER) {
        delta = -delta; // negative change to represent withdraw
    }
    char* name = message->data.material.name;

    // need write lock because we have no individual locks per material
    // and this could also add a new material
    ds_alter_mat(depotState, name, delta);
}

void execute_transfer(DepotState* depotState, Message* message) {
    Material mat = message->data.material;
    if (!is_mat_valid(mat)) {
        DEBUG_PRINT("ignoring invalid material");
        return;
    }

    ARRAY_RDLOCK(depotState->connections);
    Connection* conn = arraymap_get(depotState->connections,
            message->data.depotName);
    if (conn == NULL) {
        DEBUG_PRINTF("depot not found: %s\n", message->data.depotName);
        ARRAY_UNLOCK(depotState->connections);
        return;
    }

    DEBUG_PRINTF("withdrawing, then delivering to %s\n", conn->name);

    ds_alter_mat(depotState, mat.name, -mat.quantity);

    Message* msg = malloc(sizeof(Message));
    *msg = msg_deliver(mat.quantity, mat.name);
    chan_post(conn->outgoing, msg);
}

void execute_defer(DepotState* depotState, Message* message) {
    Message* deferMessage = message->data.deferMessage;
    switch (deferMessage->type) {
        case MSG_DELIVER:
        case MSG_WITHDRAW:
        case MSG_TRANSFER:
            break; // only these message types are valid for Defer
        default:
            DEBUG_PRINT("unsupported deferred message type");
            return; // silently ignore
    }
    ARRAY_RDLOCK(depotState->deferGroups);
    DeferGroup* dg = ds_ensure_defer_group(depotState, message->data.deferKey);
    
    ARRAY_WRLOCK(dg->messages);
    dg_add_message(dg, message->data.deferMessage);
    // deferMessage now owned by dg. delete our reference.
    message->data.deferMessage = NULL;
    ARRAY_UNLOCK(dg->messages);

    ARRAY_UNLOCK(depotState->deferGroups);

    // we can destroy message now because the sub-message has been copied
    // and its reference in *message is set to NULL.
}

void execute_execute(DepotState* depotState, Message* message) {
    // admittedly not the best naming

    ARRAY_WRLOCK(depotState->deferGroups);
    DeferGroup* dg = arraymap_get(depotState->deferGroups, 
            &message->data.deferKey);
    if (dg == NULL) {
        DEBUG_PRINT("defer key not found");
        ARRAY_UNLOCK(depotState->deferGroups);
        return;
    }
    DEBUG_PRINTF("executing defer group, key: %d\n", message->data.deferKey);
    // don't need to lock on defer group because we have an exclusive write
    // lock on the entire array of defer groups
    for (int i = 0; i < dg->messages->numItems; i++) {
        DEBUG_PRINTF("executing deferred message %d\n", i);
        Message* msg = ARRAY_ITEM(Message, dg->messages, i);
        msg_debug(msg);
        execute_message(depotState, msg);
    }

    // frees messages and destroys messages array
    dg_destroy(dg);

    // remove defer group from array of defer groups
    array_remove(depotState->deferGroups, dg);
    // and free memory
    free(dg);

    ARRAY_UNLOCK(depotState->deferGroups);
}

void execute_message(DepotState* depotState, Message* message) {
    switch (message->type) {
        case MSG_CONNECT:
            execute_connect(depotState, message);
            break;
        case MSG_IM: // ignore improperly sequenced IM
            DEBUG_PRINT("ignoring unexpected IM");
            break;
        case MSG_DELIVER:
        case MSG_WITHDRAW:
            execute_deliver_withdraw(depotState, message);
            break;
        case MSG_TRANSFER:
            execute_transfer(depotState, message);
            break;
        case MSG_DEFER:
            execute_defer(depotState, message);
            break;
        case MSG_EXECUTE:
            execute_execute(depotState, message);
            break;
        default:
            DEBUG_PRINT("invalid normal message type");
            assert(0);
    }
}

void execute_meta_message(DepotState* depotState, Message* message) {
    Connection* conn = message->data.connection;
    switch (message->type) {
        case MSG_META_SIGNAL:
            DEBUG_PRINTF("received signal %d: %s\n", message->data.signal,
                    strsignal(message->data.signal));
            ds_print_info(depotState);
            break;
        case MSG_META_CONN_NEW:
            DEBUG_PRINTF("new connection %d:%s\n", conn->port,
                    conn->name);
            if (arraymap_get(depotState->connections, conn->name) != NULL) {
                DEBUG_PRINT("connection with name already exists, ignoring.");
                break; // main thread will cleanup
            }

            bool portExists = false;
            for (int i = 0; i < depotState->connections->numItems; i++) {
                if (ARRAY_ITEM(Connection, depotState->connections, i)->port
                        == conn->port) {
                    portExists = true;
                    break;
                }
            }
            if (portExists) {
                DEBUG_PRINT("connection to port already exists, ignoring.");
                break;
            }

            DEBUG_PRINT("adding to arraymap");
            // YIELD connection to connections array
            array_add(depotState->connections, conn);
            message->data.connection = NULL;

            start_connector_thread(conn, depotState->incoming); // start thread
            break;
        case MSG_META_CONN_EOF:
            DEBUG_PRINTF("removing conn %d:%s\n", conn->port, conn->name);
            array_remove(depotState->connections, conn);
            // connection will be cleaned up by msg_destroy later
            break;
        default:
            DEBUG_PRINT("invalid meta message type");
            assert(0);
    }
}


void* reader_thread(void* readerArg) {
    ReaderData readerData = *(ReaderData*)readerArg;
    free(readerArg);
    DEBUG_PRINTF("reader thread started for %d:%s\n", readerData.port,
            readerData.name);

    MessageStatus status = MS_OK;
    while (status != MS_EOF) {
        Message msg = {0};
        status = msg_receive(readerData.readFile, &msg);
        if (status != MS_OK) {
            DEBUG_PRINT("message invalid or eof");
            continue;
        }
        // wrap received message in a heap-allocated struct
        Message* msgNew = calloc(1, sizeof(Message));
        *msgNew = msg;
        DEBUG_PRINT("sending message to incoming channel");
        // yield MessageFrom* to the incoming channel
        chan_post(readerData.incoming, msgNew);
    }
    DEBUG_PRINTF("reader thread for %d:%s reached EOF\n", readerData.port,
            readerData.name);
    return NULL;
}

void* writer_thread(void* writerArg) {
    WriterData writerData = *(WriterData*)writerArg;
    free(writerArg);
    DEBUG_PRINT("writer thread started");

    while (1) {
        Message* msg = chan_wait(writerData.outgoing);
        DEBUG_PRINT("echoing message to socket");
        msg_send(writerData.writeFile, *msg);
        msg_destroy(msg);
        free(msg);
    }
}


void* connector_thread(void* connectorArg) {
    // copy into local variable and free memory
    ConnectorData connData = *(ConnectorData*)connectorArg;
    Connection* connection = connData.connection;
    free(connectorArg);
    
    DEBUG_PRINTF("starting read/write threads for %d:%s\n", connection->port,
            connection->name);

    // starting reader thread to handle incoming messages
    ReaderData* readerData = malloc(sizeof(ReaderData));
    readerData->incoming = connData.incoming;
    readerData->readFile = connection->readFile;
    readerData->port = connection->port;
    readerData->name = connection->name;

    pthread_t readerThread;
    pthread_create(&readerThread, NULL, reader_thread, readerData);

    // start writer thread to write outgoing messages to socket
    WriterData* writerData = malloc(sizeof(WriterData));
    writerData->writeFile = connection->writeFile;
    writerData->outgoing = connection->outgoing;

    pthread_t writerThread;
    pthread_create(&writerThread, NULL, writer_thread, writerData);

    // wait for reader to reach EOF, then terminate writer thread
    pthread_join(readerThread, NULL);
    DEBUG_PRINTF("reader thread terminated, cancelling writer for %d:%s\n",
            connection->port, connection->name);

    pthread_cancel(writerThread);
    pthread_join(writerThread, NULL);

    // send meta eof message to managing thread. connection set from earlier
    Message msg = {0};
    msg.type = MSG_META_CONN_EOF;
    msg.data.connection = connection;
    Message* msgNew = malloc(sizeof(Message));
    *msgNew = msg;
    chan_post(connData.incoming, msgNew);

    DEBUG_PRINT("closing socket files and terminating connector thread");
    pthread_detach(pthread_self());
    return NULL;
}

void start_connector_thread(Connection* connection, Channel* incoming) {
    DEBUG_PRINT("starting connector thread");
    ConnectorData* connArg = calloc(1, sizeof(ConnectorData));
    connArg->connection = connection;
    connArg->incoming = incoming;

    pthread_t thread;
    pthread_create(&thread, NULL, connector_thread, connArg);
}

/* Verifies the connection on the given read/write files by sending and
 * expecting IM messages.
 * If successful, sends a CONN_NEW meta message to incoming channel. If not
 * successful, thread closes. Return value unused.
 */
void* verify_thread(void* verifyArg) {
    VerifyData verifyData = *(VerifyData*)verifyArg;
    free(verifyArg);
    DEBUG_PRINT("verifying connection");
    // prevents becoming a thread zombie. success is indicated to main via
    // channel. on failure, thread ends
    pthread_detach(pthread_self());

    Message msg = msg_im(verifyData.ourPort, verifyData.ourName);
    if (msg_send(verifyData.writeFile, msg) != MS_OK) {
        DEBUG_PRINT("sending IM failed. closing");
        msg_destroy(&msg);
        fclose(verifyData.readFile);
        fclose(verifyData.writeFile);
        return NULL;
    }
    msg_destroy(&msg); // destroy the msg_im()
    if (msg_receive(verifyData.readFile, &msg) != MS_OK ||
            msg.type != MSG_IM || !is_name_valid(msg.data.depotName)) {
        DEBUG_PRINT("invalid IM or bad depot name. closing.");
        msg_destroy(&msg);
        fclose(verifyData.readFile);
        fclose(verifyData.writeFile);
        return NULL;
    }

    Connection conn;
    conn_init(&conn, msg.data.depotPort, msg.data.depotName);
    conn_set_files(&conn, verifyData.readFile, verifyData.writeFile);
    DEBUG_PRINTF("acknowledged by %s on %d\n", conn.name, conn.port);
    msg_destroy(&msg);

    msg = (Message) {0};
    msg.type = MSG_META_CONN_NEW;
    // yields connection struct to main thread. main thread now owns it
    msg.data.connection = calloc(1, sizeof(Connection));
    *msg.data.connection = conn;
    Message* msgNew = malloc(sizeof(Message));
    *msgNew = msg;
    chan_post(verifyData.incoming, msgNew);
    
    return NULL;
}

void start_verify_thread(int port, char* name, Channel* incoming, int fd) {
    VerifyData* verifyData = malloc(sizeof(VerifyData));
    verifyData->ourPort = port;
    verifyData->ourName = name;
    verifyData->incoming = incoming;

    int fdRead = fd;
    int fdWrite = dup(fd);
    verifyData->readFile = fdopen(fdRead, "r");
    verifyData->writeFile = fdopen(fdWrite, "w");

    pthread_t verifyThread;
    pthread_create(&verifyThread, NULL, verify_thread, verifyData);
}

bool new_socket(char* port, int* fdOut, struct addrinfo** aiOut) {
    // copied with few modifications from Joel's net4.c
    struct addrinfo hints;
    memset(&hints, 0, sizeof(struct addrinfo));
    hints.ai_family = AF_INET; // ipv4
    hints.ai_socktype = SOCK_STREAM; // tcp
    hints.ai_flags = AI_PASSIVE; // server

    struct addrinfo* ai; // for address info returned by getaddrinfo
    int error = getaddrinfo("127.0.0.1", port, &hints, &ai);
    *aiOut = ai;

    if (error != 0) {
        DEBUG_PRINTF("error getaddrinfo: %s\n", gai_strerror(error));
        return false;
    }
    *fdOut = socket(AF_INET, SOCK_STREAM, 0); // 0 is default protocol
    return true;
}

bool start_active_socket(int* fdOut, char* port) {
    struct addrinfo* ai;
    int sock;
    // init new socket()
    if (!new_socket(port, &sock, &ai)) {
        return false;
    }
    if (connect(sock, ai->ai_addr, ai->ai_addrlen) != 0) {
        DEBUG_PERROR("connect()");
        freeaddrinfo(ai);
        return false;
    }
    freeaddrinfo(ai);
    *fdOut = sock;
    return true;
}

bool start_passive_socket(int* fdOut, int* portOut) {
    struct addrinfo* ai;
    int server;
    // init new socket()
    if (!new_socket(NULL, &server, &ai)) {
        return false;
    }
    if (bind(server, ai->ai_addr, ai->ai_addrlen) != 0) {
        freeaddrinfo(ai);
        DEBUG_PERROR("bind()");
        return false;
    }
    freeaddrinfo(ai);
    ai = NULL;

    // find actual address we're bound to
    struct sockaddr_in ad;
    socklen_t len = sizeof(struct sockaddr_in);
    memset(&ad, 0, len);
    if (getsockname(server, (struct sockaddr*)&ad, &len) != 0) {
        DEBUG_PERROR("getsockname()");
        return false;
    }
    DEBUG_PRINTF("bound to address %s port %d\n", inet_ntoa(ad.sin_addr),
            ntohs(ad.sin_port));

    if (listen(server, CONNECTION_QUEUE) != 0) {
        DEBUG_PERROR("listen()");
        return false;
    }

    *fdOut = server;
    *portOut = ntohs(ad.sin_port);
    return true;
}

void* main_thread(void* depotStateArg) {
    DepotState* depotState = depotStateArg;

    while (1) {
        Message* msg = chan_wait(depotState->incoming);
        int numItems;
        sem_getvalue(&depotState->incoming->numItems, &numItems);
        DEBUG_PRINTF("%d messages remain\n", numItems);
        if (msg->type >= MSG_NULL) {
            DEBUG_PRINT("received meta message");
            msg_debug(msg);
            execute_meta_message(depotState, msg);
        } else {
            DEBUG_PRINT("received normal message!");
            execute_message(depotState, msg);
        }
        msg_destroy(msg);
        free(msg);
    }
    assert(0);
}

void* server_thread(void* serverArg) {
    ServerData serverData = *(ServerData*)serverArg;
    DEBUG_PRINT("server thread started");

    while (1) {
        // listen for connections
        int fd = accept(serverData.fd, 0, 0);
        DEBUG_PRINT("accepted connection, verifying.");
        start_verify_thread(serverData.ourPort, serverData.ourName,
                serverData.incoming, fd);
    }
    assert(0);
}

DepotExitCode exec_server(DepotState* depotState) {
    DEBUG_PRINT("starting server");
    // block SIGHUP from all threads. we will pick it up via sigwait
    static sigset_t ss;
    sigemptyset(&ss);
    sigaddset(&ss, SIGHUP);
    sigaddset(&ss, SIGUSR1); // use SIGUSR1 to exit cleanly
    pthread_sigmask(SIG_BLOCK, &ss, NULL);
    ignore_sigpipe(); // also ignore SIGPIPE

    // start the server thing
    int server;
    int port;
    if (!start_passive_socket(&server, &port)) {
        DEBUG_PRINT("failed to start passive socket");
        return D_NORMAL; // no special exit code
    }
    depotState->port = port;
    printf("%d\n", port);

    // start server listener thread. data argument is allocated on stack!
    ServerData serverData;
    serverData.fd = server;
    serverData.ourName = depotState->name;
    serverData.ourPort = depotState->port;
    serverData.incoming = depotState->incoming;

    pthread_t serverThread;
    pthread_create(&serverThread, NULL, server_thread, &serverData);


    pthread_t mainThread;
    pthread_create(&mainThread, NULL, main_thread, depotState);

    while (1) {
        int sig;
        sigwait(&ss, &sig);
        DEBUG_PRINTF("signal %d: %s\n", sig, strsignal(sig));
        if (sig != SIGHUP) {
            break; // terminate if caught non SIGHUP signal
        }
        Message* msg = calloc(1, sizeof(Message));
        msg->type = MSG_META_SIGNAL;
        msg->data.signal = sig;
        chan_post(depotState->incoming, msg);
        //print_depot_info(depotState);
    }
    // terminate the program
    DEBUG_PRINT("terminating program due to signal!");

    return D_NORMAL;
}

/* Argument checks and initialises depot state. Bootstraps server listener. */
DepotExitCode exec_main(int argc, char** argv, DepotState* depotState) {
    if (argc % 2 != 0) {
        DEBUG_PRINTF("number of arguments not even: %d\n", argc);
        return D_INCORRECT_ARGS;
    }

    if (!is_name_valid(argv[1])) {
        return D_INVALID_NAME;
    }
    ds_init(depotState, argv[1]);

    for (int i = 2; i < argc; i += 2) {
        assert(i + 1 < argc);
        char* matName = argv[i];
        int quantity = parse_int(argv[i + 1]);

        if (!is_name_valid(matName)) {
            return D_INVALID_NAME;
        }
        if (quantity < 0) {
            DEBUG_PRINT("invalid quantity");
            return D_INVALID_QUANTITY;
        }

        ds_alter_mat(depotState, matName, quantity);
    }

    return exec_server(depotState);
}

// starts the program and owns state struct
int main(int argc, char** argv) {
    DepotState depotState = {0};

    DepotExitCode ret = exec_main(argc, argv, &depotState);

    ds_destroy(&depotState);

    fprintf(stderr, "%s", depot_message(ret));
    DEBUG_PRINTF("program exiting with code: %d\n", ret);
    return ret;
}

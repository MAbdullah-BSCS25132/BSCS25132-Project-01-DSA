// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

// What load balancer does:
//   1. Many clients can connect at the same time. Each one sends one string (the source code). 
//   2. Clients wait in a queue (first come, first served).
//   3. For each client, in order: make sure the server is running (start it with system() if not),
//      pass the string to the server, pass the server's reply back to the client.
//
// It does not read or understand the messages. It only moves bytes.
// The only messages it writes itself are errors ("server busy", "server failed to start").
//
// Two threads:
//   acceptor thread (acceptLoop) -> takes new clients and puts them in the queue. It never waits for the server.
//   main thread (dispatcherLoop) -> takes ONE client out of the queue and serves it completely, then the next.
//
// How the server is started:
//   system("./server 5001 &");
//   The "&" is required. Without it, system() waits until the server exits and the load balancer freezes.
//
//
// What the server program must do (the load balancer depends on this):
//   - it takes its port as the first argument, for example: ./server 5001
//   - it reads the string until the connection's write side is closed (recv returns 0)
//   - it sends its reply as [status: 1 byte][payload], then closes the connection 


#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
// #include <unistd.h>
// #include <signal.h>
// #include <sys/socket.h>
// #include <netinet/in.h>
// #include <arpa/inet.h>
#include <csignal>
#include <ctime>
#include <chrono>
#include "helper.h"
using namespace std;


// ---- Constants ----
const char *SERVER_PATH = "./server";      // the program we start when the server is down
// const int IO_BUFFER_SIZE = 64 * 1024;      // fixed buffer for moving bytes
const int LISTEN_BACKLOG = 16;             // connections the OS holds until accept() takes them
const int MAX_PENDING_CLIENTS = 8;         // clients allowed to wait in our queue. More than this get "server busy".
const int STARTUP_MAX_ATTEMPTS = 50;       // after starting the server, how many times we try to connect
const int STARTUP_POLL_INTERVAL_MS = 100;  // wait between those tries (50 x 100 ms = 5 seconds in total)
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;


// ---- Queue: here it holds client sockets (int). ----
// Implement it yourself, do not use std::queue.
template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *front, *rear;
    int count;

public:
    Queue()
    {
        front = nullptr;
        rear = nullptr;
        count = 0;
    }

    ~Queue()
    {
        while (front != nullptr)
        {
            Node *temp = front;
            front = front->next;
            delete temp;
        }
        rear = nullptr;
        count = 0;
    }

    void enqueue(const T &val)
    {
        Node *newNode = new Node;
        newNode->data = val;
        newNode->next = nullptr;

        if (rear == nullptr)
        {
            front = newNode;
            rear = newNode;
        }
        else
        {
            rear->next = newNode;
            rear = newNode;
        }
        count++;
    }

    T dequeue()
    {
        if (front == nullptr)
        {
            return T();
        }

        Node *temp = front;
        T value = front->data;
        front = front->next;

        if (front == nullptr)
        {
            rear = nullptr;
        }

        delete temp;
        count--;
        return value;
    }

    bool isEmpty()
    {
        return front == nullptr;
    }

    int size()
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;           // the connected socket
    string address;     // the client's IP as text, e.g. "127.0.0.1"
    int port;           // the client's port
    time_t arrivedAt;   // when accept() returned
};

// The queue is used by two threads, so every use of it must be inside queueMutex.
Queue<ClientInfo> pendingClients;
mutex queueMutex;
sig_atomic_t flag = 1;


// ---- Basic socket helpers ----
bool sendAll(int sock, const char *buf, size_t n) 
{
    // Reply format: [status: 1 byte][sending_material][checksum: 4 bytes].
    // send() may send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    
    size_t sent = 0;

    while (sent < n)
    {
        int r = send(sock, buf + sent, n - sent, 0);
        if (r <= 0) // 0 -> connnection closed, -1 -> error occur // r contain no of byte sent suvvessfully
            return false;
        sent += r;
    }

    return true;
}
bool sendError(int sock, const string &message)
{
    // Reply format: [status: 1 byte][payload until close][checksum: 4 bytes].
    // For an error: send one byte with value 1, then the message text. 
    
     
    char status = 1;    // status = 0 mean payload ma .tbdg file ha else not i.e error i.e 1.

    uint32_t crc = 0xFFFFFFFF;
    crc = crcUpdate(crc, &status, 1);
    crc = crcUpdate(crc, message.c_str(), (int32_t)message.size());
    uint32_t finalCrc = ~crc;

    if (!sendAll(sock, &status, 1))
        return false;
    if (!sendAll(sock, message.c_str(), message.size()))
        return false;
    if (!sendAll(sock, (char *)&finalCrc, 4))
        return false;
    return true;
}
int createListenSocket(int port)
{ 
    // Create a TCP socket with SOCK_CLOEXEC(flag).
    // set SO_REUSEADDR so we can restart the load balancer right away.
    // bind() to the port on any address, then listen(LISTEN_BACKLOG).
    // If bind or listen fails, print the error and exit, because without this socket we cannot work.

    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
    {
        cout << "could not create socket" << endl;
        exit(1);
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        cout << "Error: cannot bind to port " << port << endl;
        exit(1);
    }

    if (listen(sock, LISTEN_BACKLOG) < 0)
    {
        cout << "Error: listen failed" << endl;
        exit(1);
    }

    return sock;
}
int connectToServer(const char *host, int port)
{
    // Create a TCP socket (SOCK_CLOEXEC) and connect it to host:port.
    // Return the socket, or -1 if the connection fails 

    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return -1;

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (connect(sock, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        closeSocket(sock);
        return -1;
    }
    return sock;
}

// =============================================================================================
// PART 1: WAKING UP THE SERVER IF IT IS DOWN
// =============================================================================================


bool startServer(int port)
{
    // Build the command text: SERVER_PATH + " " + port as text + "&"
    //   Example: "./server 5001 &"
    // Call system() with it.
    // Return true if system() returned 0, otherwise false.

    #ifdef _WIN32
    string command = "start /B server.exe " + to_string(port);
    #else
        string command = string(SERVER_PATH) + " " + to_string(port) + " &";
    #endif

        int result = system(command.c_str());
        return result == 0;
}
int connectToServerWithWakeup()
{
    // 1. Try connectToServer(). If it works, return that socket. The server is alive.
    // 2. Otherwise call startServer(). If it fails, return -1.
    // 3. Try connectToServer() up to STARTUP_MAX_ATTEMPTS times 
    // 4. Return the first working socket, or -1 if the server never came up.

    int sock = connectToServer(serverHost.c_str(), serverPort);
    if (sock >= 0)
        return sock;

    if (!startServer(serverPort))
        return -1;

    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++) //50time
    {
        this_thread::sleep_for(chrono::milliseconds(STARTUP_POLL_INTERVAL_MS)); // sleep wait for 100ms then try tocnnnect

        sock = connectToServer(serverHost.c_str(), serverPort);
        if (sock >= 0)
            return sock;
    }
    return -1;
}


// =============================================================================================
// PART 2: PASSING THE STRING AND THE REPLY (one client, called by the main thread only)
// =============================================================================================


void relayOneClient(ClientInfo &c)
{
    // 1. serverSock = connectToServerWithWakeup().
    //    If it is -1: sendError "backend server failed to start", close clientSock, return.
    // 3. Client -> server: repeat recv() from clientSock into a buffer of IO_BUFFER_SIZE and sendAll() it to serverSock.
    //    Stop when recv() returns 0 (the client finished sending) or fails.
    // 4. Call shutdown(serverSock, SHUT_WR), so the server knows the string is complete.
    // 5. Server -> client: repeat recv() from serverSock and sendAll() to clientSock.
    //    Stop when recv() returns 0 (the server sent its whole reply and closed) or fails.
    // 6. Close both sockets.

    int serverSock = connectToServerWithWakeup();
    if (serverSock < 0)
    {
        sendError(c.sock, "backend server failed to start");
        closeSocket(c.sock);
        return;
    }

    char in[IO_BUFFER_SIZE];
    char out[IO_BUFFER_SIZE];

    TrailerChecker upload;
    while (true)
    {
        int r = recv(c.sock, in, IO_BUFFER_SIZE, 0);
        if (r <= 0)
            break;
        int32_t k = upload.feed(in, r, out);
        if (!sendAll(serverSock, out, k))
            break;
    }

    if (!upload.verify())
    {
        shutdown(serverSock, SHUT_WR);
        closeSocket(serverSock);
        sendError(c.sock, "upload checksum mismatch");
        closeSocket(c.sock);
        return;
    }

    sendAll(serverSock, upload.held, 4);
    shutdown(serverSock, SHUT_WR);

    TrailerChecker reply;
    while (true)
    {
        int r = recv(serverSock, in, IO_BUFFER_SIZE, 0);
        if (r <= 0)
            break;
        int32_t k = reply.feed(in, r, out);
        if (!sendAll(c.sock, out, k))
            break;
    }

    if (reply.verify())
        sendAll(c.sock, reply.held, 4);

    closeSocket(serverSock);
    closeSocket(c.sock);
}


// =============================================================================================
// PART 3: THE TWO THREADS
// =============================================================================================


void acceptLoop(int listenSock)
{
    // Repeat: accept() a new client. 
    //  If pendingClients.size() is already MAX_PENDING_CLIENTS: unlock, sendError "server busy, try again later",
    // else
    // Lock queueMutex.
    //   close the client, and go on.
    //   Otherwise enqueue the client socket.
    // Never wait for the server here, so new clients are always accepted quickly.
    // If accept() fails, print the error and keep going (do not end the thread).

    while (flag)
    {
        sockaddr_in clientAddr;
        socklen_t len = sizeof(clientAddr);
        int clientSock = accept(listenSock, (sockaddr *)&clientAddr, &len);
        if (clientSock < 0)
        {
            cout << "accept failed" << endl;
            continue;
        }
#ifdef _WIN32
        SetHandleInformation((HANDLE)clientSock, HANDLE_FLAG_INHERIT, 0);
#else
        fcntl(clientSock, F_SETFD, FD_CLOEXEC);
#endif
        ClientInfo c;
        c.sock = clientSock;
        c.address = inet_ntoa(clientAddr.sin_addr);
        c.port = ntohs(clientAddr.sin_port);
        c.arrivedAt = time(nullptr);

        queueMutex.lock();
        if (pendingClients.size() >= MAX_PENDING_CLIENTS)
        {
            queueMutex.unlock();
            sendError(clientSock, "server busy, try again later");
            closeSocket(clientSock);
        }
        else
        {
            pendingClients.enqueue(c);
            queueMutex.unlock();
        }
    }
}

void dispatcherLoop()
{
    // This runs in the main thread and never ends.
    // Repeat:
    //   Lock queueMutex, take ONE client out of the queue if there is one, unlock.
    //   If we got a client: call relayOneClient(). The next client is taken out of the queue only after this one is finished.

    const int timeoutSeconds = 30;

    while (flag)
    {
        ClientInfo c;
        bool got = false;

        queueMutex.lock();
        if (!pendingClients.isEmpty())
        {
            c = pendingClients.dequeue();
            got = true;
        }
        queueMutex.unlock();

        if (!got)
        {
            this_thread::sleep_for(chrono::milliseconds(50));
            continue;
        }

        if (time(nullptr) - c.arrivedAt > timeoutSeconds)
        {
            closeSocket(c.sock);
            continue;
        }

        relayOneClient(c);
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}
int main(int argc, char *argv[])
{

    // signal(SIGINT, handler); // whenever ctrl+c will be pressed to exit the program the flag wll be set so insead of using infinite loop you can loop on flag to safely close things and exit
    // 2. listenSock = createListenSocket(LB_PORT).
    // 3. Start a thread that runs acceptLoop(listenSock) and detach it.
    // 4. Call dispatcherLoop().

    if (argc >= 2)
        serverHost = argv[1];
    if (argc >= 3)
        serverPort = atoi(argv[2]);

    signal(SIGINT, handler);

    int listenSock = createListenSocket(lbPort);
    cout << "Load balancer listening on port " << lbPort << endl;

    thread acceptor(acceptLoop, listenSock);
    acceptor.detach();

    dispatcherLoop();

    closeSocket(listenSock);
    return 0;
}
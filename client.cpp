// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <fstream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
//#include <unistd.h>
//#include <signal.h>
#include <vector>
//#include <sys/socket.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char *PROGRAM_NAME = "ttdb";
const char *PROMPT = "ttdb> ";
// const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char *TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

//const int MAX_VARS_PER_FRAME = 16;
//const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
// struct Variable
// {
//     string name;
//     int32_t value;
// };
// struct Frame
// {
//     string func_name;
//     int32_t argc;
//     Variable argv[MAX_VARS_PER_FRAME];
//     int32_t returnLine;
//     Variable locals[MAX_VARS_PER_FRAME];
//     int32_t localCount;
// };
// struct Snapshot
// {
//     Frame callStack[MAX_STACK_DEPTH];
//     int32_t stackDepth;
// };
// struct TTDBHeader 
// {
//     char magic[4]; // "TTDB"
//     int32_t version;
//     int32_t stepCount;
//     int64_t indexOffset;
// };

// ---- Structs only the client needs ----
struct TdbgReader
{
    ifstream file;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep; 
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char *buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.

    size_t got = 0;

    while (got < n)
    {
        int r = recv(sock, buf + got, n - got, 0);
        if (r <= 0)
            return false;
        got = got + r;
    }
    return true;
}
bool sendAll(int sock, const char *buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.

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

int connectToHost(const char *host, int port) // ***
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.

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
bool uploadSource(int sock, const char *sourcePath) 
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.

    ifstream file(sourcePath, ios::binary);
    if (!file)
        return false;

    uint32_t crc = 0xFFFFFFFF;

    char status = 0;
    if (!sendAll(sock, &status, 1))
        return false;
    crc = crcUpdate(crc, &status, 1);

    char buf[IO_BUFFER_SIZE];
    while (true)
    {
        file.read(buf, IO_BUFFER_SIZE);
        int32_t got = (int32_t)file.gcount();
        if (got == 0)
            break;

        if (!sendAll(sock, buf, got))
            return false;
        crc = crcUpdate(crc, buf, got);
    }

    uint32_t finalCrc = ~crc;
    if (!sendAll(sock, (char *)&finalCrc, 4))
        return false;

    shutdown(sock, SHUT_WR);
    return true;
}
bool receiveResponse(int sock, const char *outTdbgPath, string &outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.

    TrailerChecker checker;
    ofstream file(outTdbgPath, ios::binary);
    if (!file)
    {
        outError = "cannot create file";
        return false;
    }

    char in[IO_BUFFER_SIZE];
    char out[IO_BUFFER_SIZE];
    bool gotStatus = false;
    char status = 0;
    string errorText = "";

    while (true)
    {
        int r = recv(sock, in, IO_BUFFER_SIZE, 0);
        if (r <= 0)
            break;

        int32_t k = checker.feed(in, r, out);
        int32_t start = 0;

        if (!gotStatus && k > 0)
        {
            status = out[0];
            gotStatus = true;
            start = 1;
        }

        if (status == 0)
            file.write(out + start, k - start);
        else
            errorText = errorText + string(out + start, k - start);
    }

    file.close();

    if (!checker.verify())
    {
        remove(outTdbgPath);
        outError = "checksum mismatch, response corrupted";
        return false;
    }
    if (!gotStatus)
    {
        remove(outTdbgPath);
        outError = "empty reply from server";
        return false;
    }
    if (status == 1)
    {
        remove(outTdbgPath);
        outError = errorText;
        return false;
    }
    return true;
}

// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================

bool openTdbg(const char *path, TdbgReader &r)
{
    r.file.open(path, ios::binary);
    if(!r.file)
        return false; 

    r.file.read(r.header.magic, 4);
    r.file.read((char *)&r.header.version, sizeof(r.header.version));
    r.file.read((char *)&r.header.stepCount, sizeof(r.header.stepCount));
    r.file.read((char *)&r.header.indexOffset, sizeof(r.header.indexOffset));

    if (!r.file)
    {
       r.file.close();
       return false;
    }
    
    r.file.seekg(0, ios::end);
    int64_t fileSize = r.file.tellg();

    bool magicOk = (r.header.magic[0] == 'T' && r.header.magic[1] == 'T' && r.header.magic[2] == 'D' && r.header.magic[3] == 'B');
    if (!magicOk || r.header.stepCount <= 0 || r.header.indexOffset < 20 || r.header.indexOffset + (int64_t)r.header.stepCount * 8 != fileSize)
    {
        r.file.close();
        return false;
    }

    r.index.resize(r.header.stepCount);
    r.file.seekg(r.header.indexOffset);
    r.file.read((char *)&r.index[0], (int64_t)r.header.stepCount * 8);

    for (int32_t i = 0; i < r.header.stepCount; i++)
    {
        if (r.index[i] < 20 || r.index[i] >= r.header.indexOffset)
        { 
            r.file.close(); r.index.clear(); return false;
        }
    }
    return true;
}
void closeTdbg(TdbgReader &r)
{
    r.file.close();
    r.index.clear();
}

Snapshot *readSnapshotAt(TdbgReader &r, int32_t step)
{
    if (step < 0 || step >= r.header.stepCount)
    {
        return nullptr;
    }

    r.file.clear();
    r.file.seekg(r.index[step]);

    Snapshot *snap = new Snapshot;

    char buf[256];
    int32_t len;

    r.file.read((char *)&snap->stackDepth, sizeof(snap->stackDepth));

    if (!r.file || snap->stackDepth < 0 || snap->stackDepth > MAX_STACK_DEPTH)
    {
        delete snap;
        return nullptr;
    }

    for (int32_t i = 0; i < snap->stackDepth; i++)
    {
        Frame &fr = snap->callStack[i];

        r.file.read((char *)&len, sizeof(len));

        if (!r.file || len < 0 || len > 255)
        {
            delete snap;
            return nullptr;
        }

        r.file.read(buf, len);
        fr.func_name = string(buf, len);

        r.file.read((char *)&fr.argc, sizeof(fr.argc));

        if (!r.file || fr.argc < 0 || fr.argc > MAX_VARS_PER_FRAME)
        {
            delete snap;
            return nullptr;
        }

        for (int32_t j = 0; j < fr.argc; j++)
        {
            r.file.read((char *)&len, sizeof(len));

            if (!r.file || len < 0 || len > 255)
            {
                delete snap;
                return nullptr;
            }

            r.file.read(buf, len);
            fr.argv[j].name = string(buf, len);

            r.file.read((char *)&fr.argv[j].value, sizeof(fr.argv[j].value));
        }

        r.file.read((char *)&fr.returnLine, sizeof(fr.returnLine));

        r.file.read((char *)&fr.localCount, sizeof(fr.localCount));

        if (!r.file || fr.localCount < 0 || fr.localCount > MAX_VARS_PER_FRAME)
        {
            delete snap;
            return nullptr;
        }

        for (int32_t j = 0; j < fr.localCount; j++)
        {
            r.file.read((char *)&len, sizeof(len));

            if (!r.file || len < 0 || len > 255)
            {
                delete snap;
                return nullptr;
            }

            r.file.read(buf, len);
            fr.locals[j].name = string(buf, len);

            r.file.read((char *)&fr.locals[j].value, sizeof(fr.locals[j].value));
        }
    }

    if (!r.file)
    {
        delete snap;
        return nullptr;
    }

    return snap;
}


// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================

void initSession(DebugSession &s)
{
   
}
void closeSession(DebugSession &s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if(s.active_flag)
    {
        closeTdbg(s.reader);
        s.active_flag = false;
    }
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s); // defined in Part E

bool showCurrent(DebugSession &s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.

    Snapshot *snap = readSnapshotAt(s.reader, (int32_t)s.currentStep);

    if (snap == nullptr)
    {
        cout << "Errored/Damaged .tdbg file" << endl;
        return false;
    }

    printSnapshot((int32_t)s.currentStep, s.reader.header.stepCount, *snap);

    delete snap;
    return true;

}
bool moveBy(DebugSession &s, int32_t delta)
{
    int64_t newStep = s.currentStep + delta;

    if (newStep < 0)
    {
        cout << "Already at the first step" << endl;
        return false;
    }

    if (newStep >= s.reader.header.stepCount)
    {
        cout << "Already at the last step" << endl;
        return false;
    }

    s.currentStep = newStep;
    return showCurrent(s);
}
bool cmdStart(DebugSession &s, const string &sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).

    static bool useFirst = true;
    const char *newPath = useFirst ? TDBG_FILE : "ttdb_other.tdbg";

    int sock = connectToHost(LB_HOST.c_str(), LB_PORT);
    if (sock < 0)
    {
        cout << "cannot connect to the load balancer" << endl;
        return false;
    }

    if (!uploadSource(sock, sourcePath.c_str()))
    {
        cout << "cannot send " << sourcePath << endl;
        closeSocket(sock);
        return false;
    }

    string error = "";
    bool ok = receiveResponse(sock, newPath, error);
    closeSocket(sock);
    if (!ok)
    {
        cout << error << endl;
        return false;
    }

    TdbgReader newReader;
    if (!openTdbg(newPath, newReader))
    {
        cout << "server sent a damaged .tdbg file" << endl;
        remove(newPath);
        return false;
    }

    if (s.active_flag)
        closeTdbg(s.reader);
    s.reader = move(newReader);
    s.currentStep = 0;
    s.active_flag = 1;
    useFirst = !useFirst;

    return showCurrent(s);
}
bool cmdNext(DebugSession &s)
{
    if (!s.active_flag)
    {
        cout << "No session rn. Use : start <source.bin>" << endl;
        return false;
    }
    return moveBy(s, 1);
}

bool cmdPrv(DebugSession &s)
{
    if (!s.active_flag)
    {
        cout << "No session rn. Use : start <source.bin>" << endl;
        return false;
    }
    return moveBy(s, -1);
}
bool cmdJmpNext(DebugSession &s, int32_t n)
{
    if (!s.active_flag)
    {
        cout << "No session active. Use : start <source.bin>" << endl;
        return false;
    }
    if (n <= 0)
    {
        cout << "Jump must be a positive number" << endl;
        return false;
    }
    return moveBy(s, n);
}

bool cmdJmpPrv(DebugSession &s, int32_t n)
{
    if (!s.active_flag)
    {
        cout << "No session active. Use : start <source.bin>" << endl;
        return false;
    }
    if (n <= 0)
    {
        cout << "Jump must be a positive number" << endl;
        return false;
    }
    return moveBy(s, -n);
}

// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot &s)
{
     cout << "Step " << step + 1 << " of " << totalSteps << endl;

    for (int32_t i = 0; i < s.stackDepth; i++)
    {
        const Frame &f = s.callStack[i];

        cout << f.func_name << "(";
        for (int32_t j = 0; j < f.argc; j++)
        {
            if (j > 0)
                cout << ", ";
            cout << f.argv[j].name << "=" << f.argv[j].value;
        }
        cout << ")";

        if (i == s.stackDepth - 1)
            cout << "   <-- current";
        cout << endl;

        for (int32_t j = 0; j < f.localCount; j++)
            cout << "    " << f.locals[j].name << " = " << f.locals[j].value << endl;
    }
}
void printHelp()
{
    // Print the commands

    cout << "Commands :" << endl;
    cout << "  start <source.bin>  :   send the program and show step 1" << endl;
    cout << "  next                :   go one step forward" << endl;
    cout << "  prv                 :   go one step back" << endl;
    cout << "  jmp next <n>        :   go n steps forward" << endl;
    cout << "  jmp prv <n>         :   go n steps back" << endl;
    cout << "  help                :   show this list" << endl;
    cout << "  quit / exit         :   leave the debugger" << endl;
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession &s, const string &line, bool &quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.

    string words[10];
    int32_t count = 0;
    string current = "";

    for (int32_t i = 0; i < (int32_t)line.size(); i++)
    {
        if (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')
        {
            if (current != "")
            {
                if (count < 10)
                {
                    words[count] = current;
                    count++;
                }
                current = "";
            }
        }
        else
        {
            current = current + line[i];
        }
    }
    if (current != "" && count < 10)
    {
        words[count] = current;
        count++;
    }

    int32_t first = 0;
    if (count > 0 && words[0] == "ttdb")
        first = 1;

    int32_t n = count - first;
    if (n == 0)
        return false;

    string cmd = words[first];

    if (cmd == "start" && n == 2)
        return cmdStart(s, words[first + 1]);

    if (cmd == "next" && n == 1)
        return cmdNext(s);

    if (cmd == "prv" && n == 1)
        return cmdPrv(s);

    if (cmd == "jmp" && n == 3)
    {
        int32_t num = atoi(words[first + 2].c_str());
        if (words[first + 1] == "next")
            return cmdJmpNext(s, num);
        if (words[first + 1] == "prv")
            return cmdJmpPrv(s, num);
    }

    if (cmd == "help" && n == 1)
    {
        printHelp();
        return true;
    }

    if ((cmd == "quit" || cmd == "exit") && n == 1)
    {
        quitRequested = true;
        return true;
    }

    cout << "unknown command" << endl;
    printHelp();
    return false;
}
int main(int argc, char *argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.

    if (argc >= 2)
        LB_HOST = argv[1];
    if (argc >= 3)
        LB_PORT = atoi(argv[2]);

    DebugSession session;
    initSession(session);

    cout << "   = = = Time - Travel Debugger = = = \n";
    printHelp();

    bool quitRequested = false;
    string line;

    while (true)
    {
        if (quitRequested)
            break;

        cout << "ttdb> ";

        if (!getline(cin, line))
            break;

        runCommand(session, line, quitRequested);
    }

    closeSession(session);
    return 0;
}

#ifndef _WIN32
#include <unistd.h>
#include <sys/socket.h>
#endif

// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include "helper.h"
#include <string>
#include <cstdint>
#include <fstream>
//#include <unistd.h>
// #include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { // initialize the stack
        top = nullptr;
        count = 0;
    }
    ~Stack()
    {
        while (top != nullptr)
        {
            Node* old = top;
            top = top->next;
            delete old;
        }
    }
    void push(const T &val)
    {
        // pushes the value on the stack if max limit is not reached yet.
        if (count >= MAX_STACK_DEPTH)
        {
            cout << "Error : Stack overflow !\n";
            return;
        }
        Node* n = new Node;
        n->data = val;
        n->next = top;
        top = n;
        count++;
    }
    T pop()
    {
        // pop the top value on the stack
        if (top == nullptr)
        {
            cout << "Stack is empty\n";
            return T();
        }
        Node* old = top;
        T val = old->data;
        top = top->next;
        delete old;
        count--;
        return val;
    }
    T &peek()
    {
        // returns the top value on the stack
        return top->data;
    }
    bool isEmpty()
    {
        return (top == nullptr);
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen) // int32_t = 4 byte integer
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        int32_t ct = 0;
        Node* cur = top;
        while (cur != nullptr && ct < maxLen)
        {
            out[ct] = cur->data;
            cur = cur->next;
            ct++;
        }
        return ct;
    }
};


// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline()
    {
        head = tail = nullptr;
        stepCount = 0;
    }
    void record(Snapshot *s)
    {
        // add record in the timeline
        TimelineNode* n = new TimelineNode;
        n->data = s;
        n->next = nullptr;
        n->prev = tail;
        if(tail == nullptr)
            head = n;
        else
            tail->next = n;
        tail = n;
        stepCount++;
    }
    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

void writeHeader(ofstream& file, const TTDBHeader &h)
{
    file.write((char*)&h.magic, sizeof(char) * 4);
    file.write((char*)&h.version, sizeof(int32_t));

    // placeholder for other two data members
    file.write((char*)&h.stepCount, sizeof(int32_t));
    file.write((char*)&h.indexOffset, sizeof(int64_t));
}


// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    // reads the next nonblank line
    string line;
    while (getline(in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();

        size_t start = 0;
        while (start < line.size() && line[start] == ' ')
            start++;
        line = line.substr(start);

        if (!line.empty())
        {
            out = line;
            return true;
        }
    }
    return false;
}
string firstWord(const string &line)
{
    // returns first word from the input string
    size_t i = 0;
    while (i < line.size() && line[i] != ' ')
        i++;
    return line.substr(0, i);
}
string secondWord(const string &line)
{
    // returns the second word
    size_t i = 0;
    while(i < line.size() && line[i] != ' ')
        i++;
    while(i < line.size() && line[i] == ' ')
        i++;
    size_t start = i;
    while(i < line.size() && line[i] != ' ')
        i++;

    return line.substr(start, i - start);
}
bool validateProgram(const char *sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
    ifstream in(sourcePath);
    if(!in)
    {
        cout << "Error : Source file not found !\n";
        return false;
    }
    Stack <string> st;
    string line;

    while(readSourceLine(in, line))
    {
        string startWord = firstWord(line);
        if(startWord == "func")
        {
            if(st.isEmpty())
                st.push(startWord);
            else if(!st.isEmpty())
            {
                cout << "Error : Nested function found !\n";
                return false;
            }
        }
        else if(startWord == "func_end")
        {
            if(st.isEmpty())
            {
                cout << "Error : FOund func_end without func (i.e no starting point)!\n";
                return false;
            }
            st.pop();
        }
    }
    if(!st.isEmpty())
    {
        cout << "Error : function " << st.peek() << " dont have func_end !\n";
        return false;
    }

    return true;    
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(ofstream &f, int64_t offsetField, const string &text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    int64_t startPos = f.tellp(); // tells position of pointer in file
    int32_t size = text.size();

    f.write((char*)&offsetField, sizeof(offsetField));
    f.write((char*)&size, sizeof(size));
    f.write(text.c_str(), sizeof(char)*size);

    return startPos;
}
int64_t readResolveRecord(ifstream &f, string &outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
    int64_t offsetField;
    int32_t size;
    char text[512];

    if(!f.read((char*)&offsetField, sizeof(offsetField)))
        return -1;

    f.read((char*)&size, sizeof(size));
    f.read(text, sizeof(char)*size);
    text[size] = '\0';

    outText = text;
    return offsetField;
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    // if there is no main return the error 
    ifstream in(sourcePath);
    ofstream out(resolveBinPath, ios::binary);
    if (!in || !out)
    {
        cout << "Error: cannot open files" << endl;
        return -1;
    }
    string line;
    while (readSourceLine(in, line))
    {
        int64_t pos = out.tellp();
        string word = firstWord(line);

        if (word == "func" && funcCount < MAX_FUNCS)
        {
            funcArray[funcCount].funcName = secondWord(line);
            funcArray[funcCount].byteOffsetInResolveBin = pos;
            funcCount++;
        }
        else if (word == "call" && patchCount < MAX_PATCHES)
        {
            patches[patchCount].byteOffsetOfOffsetField = pos;
            patches[patchCount].targetFuncName = secondWord(line);
            patchCount++;
        }
        writeResolveRecord(out, pos, line);
    }
    out.close();

    fstream f(resolveBinPath, ios::in | ios::out | ios::binary);
    for (int32_t i = 0; i < patchCount; i++)
    {
        int64_t target = -1;
        for (int32_t j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
                target = funcArray[j].byteOffsetInResolveBin;
        }

        if (target == -1)
        {
            cout << "Error : Function is undefined for this call !'" << patches[i].targetFuncName << "'" << endl;
            return -1;
        }

        f.seekp(patches[i].byteOffsetOfOffsetField);
        f.write((char*)&target, sizeof(target));
    }
    f.close();

    for (int32_t j = 0; j < funcCount; j++)
    {
        if (funcArray[j].funcName == "main")
            return funcArray[j].byteOffsetInResolveBin;
    }

    cout << "Error : Main function does not exist !\n";
    return -1;    

}

// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
    int32_t count = 0; // actual words in line
    int32_t i = 0; // aik poori line k ander position 

    while (i < line.length() && count < maxTokens)
    {
        while (i < line.length() && line[i] == ' ')
            i++;

        if (i == line.length())
            break;

        int32_t start = i;
        while (i < line.length() && line[i] != ' ')
            i++;
        tokens[count].text = line.substr(start, i - start);
        if (count == 0)
            tokens[count].type = KEYWORD;
        else if (count == 1)
            tokens[count].type = IDENTIFIER;
        else
            tokens[count].type = PARAM;
        count++;
    }
    return count;
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack givenif iwanna
    Snapshot* snap = new Snapshot;

    snap->stackDepth = callStack.snapshot_into(snap->callStack, MAX_STACK_DEPTH);
    
    return snap;
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action

    ifstream file(resolveBinPath, ios::binary);

    if (!file)
    {
        cout << "Error : cant open resolve.bin" << endl;
        return;
    }

    Stack<Frame> callStack;

    Frame mainFrame;
    mainFrame.func_name = "main";
    mainFrame.argc = 0;
    mainFrame.returnLine = -1;
    mainFrame.localCount = 0;

    callStack.push(mainFrame);

    string line;
    Token tokens[MAX_TOKENS];

    file.seekg(mainOffset);
    readResolveRecord(file, line);

    while (!callStack.isEmpty())
    {
        int64_t linePosition = file.tellg();
        int64_t targetOffset = readResolveRecord(file, line);

        if (targetOffset == -1)
            break;

        int32_t tokenCount = tokenizeLine(line, tokens, MAX_TOKENS);
        string command = tokens[0].text;

        Frame &currentFrame = callStack.peek();

        int32_t values[MAX_TOKENS] = {0};

        for (int32_t i = 1; i < tokenCount; i++)
        {
            bool found = false;
            for (int32_t j = 0; j < currentFrame.argc; j++)
            {
                if (currentFrame.argv[j].name == tokens[i].text)
                {
                    values[i] = currentFrame.argv[j].value;
                    found = true;
                    break;
                }
            }

            if (!found)
            {
                for (int32_t j = 0; j < currentFrame.localCount; j++)
                {
                    if (currentFrame.locals[j].name == tokens[i].text)
                    {
                        values[i] = currentFrame.locals[j].value;
                        found = true;
                        break;
                    }
                }
            }

            if (!found)
            {
                char firstChar = tokens[i].text[0];

                if ((firstChar >= '0' && firstChar <= '9') || firstChar == '-')
                {
                    values[i] = stoi(tokens[i].text);
                }
            }
        }

        bool shouldStore = false;
        int32_t result = 0;

        if (command == "set")
        {
            result = values[2];
            shouldStore = true;
        }
        else if (command == "add")
        {
            result = values[1] + values[2];
            shouldStore = true;
        }
        else if (command == "sub")
        {
            result = values[1] - values[2];
            shouldStore = true;
        }
        else if (command == "mul")
        {
            result = values[1] * values[2];
            shouldStore = true;
        }
        else if (command == "div")
        {
            if (values[2] == 0)
            {
                cout << "Error: divide by zero" << endl;
                return;
            }

            result = values[1] / values[2];
            shouldStore = true;
        }
        else if (command == "call")
        {
            if (callStack.depth() >= MAX_STACK_DEPTH)
            {
                cout << "Error: stack overflow" << endl;
                return;
            }

            file.seekg(targetOffset);

            string functionLine;
            readResolveRecord(file, functionLine);

            Token functionTokens[MAX_TOKENS];

            int32_t functionTokenCount = tokenizeLine(functionLine, functionTokens, MAX_TOKENS);

            if (functionTokenCount != tokenCount)
            {
                cout << "Error : wrong number of arguments for " << tokens[1].text << endl;
                return;
            }

            Frame newFrame;

            newFrame.func_name = functionTokens[1].text;
            newFrame.argc = tokenCount - 2;
            newFrame.returnLine = linePosition;
            newFrame.localCount = 0;

            for (int32_t i = 0; i < newFrame.argc; i++)
            {
                newFrame.argv[i].name = functionTokens[i + 2].text;
                newFrame.argv[i].value = values[i + 2];
            }

            callStack.push(newFrame);
        }
        else if (command == "func_end")
        {
            Frame finishedFrame = callStack.pop();

            if (callStack.isEmpty())
            {
                break;
            }

            file.clear();
            file.seekg(finishedFrame.returnLine);

            string callLine;
            readResolveRecord(file, callLine);

            Token callTokens[MAX_TOKENS];

            tokenizeLine(callLine, callTokens, MAX_TOKENS);

            Frame &callerFrame = callStack.peek();

            for (int32_t i = 0; i < finishedFrame.argc; i++)
            {
                string callerVariable = callTokens[i + 2].text;
                int32_t newValue = finishedFrame.argv[i].value;

                for (int32_t j = 0; j < callerFrame.argc; j++)
                {
                    if (callerFrame.argv[j].name == callerVariable)
                    {
                        callerFrame.argv[j].value = newValue;
                    }
                }

                for (int32_t j = 0; j < callerFrame.localCount; j++)
                {
                    if (callerFrame.locals[j].name == callerVariable)
                    {
                        callerFrame.locals[j].value = newValue;
                    }
                }
            }
        }

        if (shouldStore)
        {
            bool placed = false;

            for (int32_t i = 0; i < currentFrame.argc; i++)
            {
                if (currentFrame.argv[i].name == tokens[1].text)
                {
                    currentFrame.argv[i].value = result;
                    placed = true;
                    break;
                }
            }

            if (!placed)
            {
                for (int32_t i = 0; i < currentFrame.localCount; i++)
                {
                    if (currentFrame.locals[i].name == tokens[1].text)
                    {
                        currentFrame.locals[i].value = result;
                        placed = true;
                        break;
                    }
                }
            }

            if (!placed && currentFrame.localCount < MAX_VARS_PER_FRAME)
            {
                int32_t i = currentFrame.localCount;

                currentFrame.locals[i].name = tokens[1].text;
                currentFrame.locals[i].value = result;

                currentFrame.localCount++;
            }
        }

        Snapshot *snapshot = buildSnapshot(callStack);
        timeline.record(snapshot);
    }
}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    // placeholder for header
    // index array of the size of stepcount from the timeline
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
    // after timeline add the index array i the file
    // update the header

    ofstream file(tdbgPath, ios::binary);

    if (!file)
    {
        cout << "Error : cannot found file : " << tdbgPath << " ! "<< endl;
        return;
    }

    int32_t stepCount = timeline.getStepCount();

    TTDBHeader header;
    header.magic[0] = 'T';
    header.magic[1] = 'T';
    header.magic[2] = 'D';
    header.magic[3] = 'B';
    header.version = 1;
    header.stepCount = stepCount;
    header.indexOffset = 0;

    writeHeader(file, header);

    int64_t *index = new int64_t[stepCount];

    TimelineNode *node = timeline.begin();
    int32_t step = 0;

    while (node != nullptr)
    {
        Snapshot* snap = node->data;

        index[step] = file.tellp();

        file.write((char*)&snap->stackDepth, sizeof(snap->stackDepth));

        for (int32_t i = 0; i < snap->stackDepth; i++)
        {
            Frame& fr = snap->callStack[i];

            int32_t len = fr.func_name.size();
            file.write((char*)&len, sizeof(len));
            file.write(fr.func_name.c_str(), len);

            file.write((char*)&fr.argc, sizeof(fr.argc));

            for (int32_t j = 0; j < fr.argc; j++)
            {
                len = fr.argv[j].name.size();
                file.write((char*)&len, sizeof(len));
                file.write(fr.argv[j].name.c_str(), len);
                file.write((char*)&fr.argv[j].value, sizeof(fr.argv[j].value));
            }

            file.write((char*)&fr.returnLine, sizeof(fr.returnLine));

            file.write((char*)&fr.localCount, sizeof(fr.localCount));

            for (int32_t j = 0; j < fr.localCount; j++)
            {
                len = fr.locals[j].name.size();
                file.write((char*)&len, sizeof(len));
                file.write(fr.locals[j].name.c_str(), len);
                file.write((char*)&fr.locals[j].value, sizeof(fr.locals[j].value));
            }
        }

        step++;
        node = node->next;
    }

    int64_t indexOffset = file.tellp();

    file.write((char*)index, stepCount * sizeof(int64_t));

    header.indexOffset = indexOffset;

    file.seekp(0);
    writeHeader(file, header);

    delete[] index;

    file.close();
}
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
const int LISTEN_BACKLOG = 16;
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
// // main section
// int32_t main()
// {

//     if (!validateProgram("source.bin"))
//     {
//         // send an error response instead of a .tdbg file
//         cout << "Validation failed, session.tdbg not created !" << endl;
//         return 1;
//     }

//     int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
//     if (mainOffset == -1)
//     {
//         cout << "Resolve failed, session.tdbg not created !" << endl;
//         return 1;
//     }

//     Timeline timeline;
//     executeProgram("resolve.bin", mainOffset, timeline);

//     writeTdbg(timeline, "session.tdbg");

//     cout << "session.tdbg created successfully !" << endl;

//     return 0;
// }
int32_t main(int argc, char *argv[])
{
    int port = 5001;
    if (argc >= 2)
        port = atoi(argv[1]);

    int listenSock = createListenSocket(port);
    cout << "Server listening on port " << port << endl;

    char in[IO_BUFFER_SIZE];
    char out[IO_BUFFER_SIZE];

    while (true)
    {
        int sock = accept(listenSock, nullptr, nullptr);
        if (sock < 0)
            continue;

        // 1. upload lo, source.bin me likho
        TrailerChecker checker;
        ofstream src("source.bin", ios::binary);
        bool gotStatus = false;

        while (true)
        {
            int r = recv(sock, in, IO_BUFFER_SIZE, 0);
            if (r <= 0)
                break;

            int32_t k = checker.feed(in, r, out);
            int32_t start = 0;
            if (!gotStatus && k > 0)
            {
                gotStatus = true;
                start = 1;                      // pehla byte status hai, file me nahi
            }
            src.write(out + start, k - start);
        }
        src.close();

        if (!gotStatus || !checker.verify())
        {
            sendError(sock, "upload checksum mismatch");
            closeSocket(sock);
            continue;
        }

        // 2. Phase 1
        if (!validateProgram("source.bin"))
        {
            sendError(sock, "invalid program: func and func_end do not match");
            closeSocket(sock);
            continue;
        }

        int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");
        if (mainOffset < 0)
        {
            sendError(sock, "no main function, or call to an undefined function");
            closeSocket(sock);
            continue;
        }

        Timeline timeline;
        executeProgram("resolve.bin", mainOffset, timeline);
        if (timeline.getStepCount() == 0)
        {
            sendError(sock, "program could not be executed");
            closeSocket(sock);
            continue;
        }

        writeTdbg(timeline, "session.tdbg");

        // 3. jawab: [status 0][session.tdbg][CRC]
        ifstream tdbg("session.tdbg", ios::binary);
        char status = 0;
        uint32_t crc = 0xFFFFFFFF;

        sendAll(sock, &status, 1);
        crc = crcUpdate(crc, &status, 1);

        while (true)
        {
            tdbg.read(in, IO_BUFFER_SIZE);
            int32_t got = (int32_t)tdbg.gcount();
            if (got == 0)
                break;
            sendAll(sock, in, got);
            crc = crcUpdate(crc, in, got);
        }

        uint32_t finalCrc = ~crc;
        sendAll(sock, (char *)&finalCrc, 4);
        closeSocket(sock);
    }
}
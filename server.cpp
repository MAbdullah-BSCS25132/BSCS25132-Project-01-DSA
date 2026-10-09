// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)


#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <unistd.h>
#include <sys/socket.h>
#include <cstdint>
#include <cstdio>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
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

// Core structs
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(ofstream& file, const TTDBHeader &h)
{
    file.write((char*)&h.magic, sizeof(char) * 4);
    file.write((char*)&h.version, sizeof(int32_t));

    // placeholder for other two data members
    file.write((char*)&h.stepCount, sizeof(int32_t));
    file.write((char*)&h.indexOffset, sizeof(int64_t));
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};



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
        cout << "Error : File not found !\n";
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
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}
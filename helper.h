#pragma once

#include <string>
#include <cstdint>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define closeSocket closesocket
#define MSG_NOSIGNAL 0
#define SOCK_CLOEXEC 0
#define SHUT_WR SD_SEND
struct WinsockStarter
{
    WinsockStarter()
    {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
};
static WinsockStarter winsockStarter;
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define closeSocket close
#endif

using namespace std;


const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t IO_BUFFER_SIZE = 64 * 1024;
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
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin;
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField;
    string targetFuncName;
};

uint32_t crcUpdate(uint32_t crc, const char *data, int32_t n)
{
    for (int32_t i = 0; i < n; i++)
    {
        crc = crc ^ (uint8_t)data[i];
        for (int32_t k = 0; k < 8; k++)
        {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc = crc >> 1;
        }
    }
    return crc;
}

struct TrailerChecker
{
    char held[4];
    int32_t heldCount;
    uint32_t crc;

    TrailerChecker()
    {
        heldCount = 0;
        crc = 0xFFFFFFFF;
    }

    int32_t feed(const char *in, int32_t n, char *out)
    {
        int32_t outCount = 0;
        for (int32_t i = 0; i < n; i++)
        {
            if (heldCount == 4)
            {
                out[outCount] = held[0];
                outCount++;
                crc = crcUpdate(crc, held, 1);
                held[0] = held[1];
                held[1] = held[2];
                held[2] = held[3];
                heldCount = 3;
            }
            held[heldCount] = in[i];
            heldCount++;
        }
        return outCount;
    }

    bool verify()
    {
        if (heldCount != 4)
            return false;
        uint32_t got;
        memcpy(&got, held, 4);
        return got == ~crc;
    }
};
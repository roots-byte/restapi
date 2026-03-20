/* 
 * Copyright (c) 2026 Jonáš Rys
 * Licensed under the MIT License. See LICENSE file for details.
 */
/**
 * @file restapi.c
 * @brief Core implementation of REST API socket communication.
 */

#include "restapi_compat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#include "restapi.h"
#define THREAD_POOL_IMPLEMENTATION
#include "thread_pool.h"
#include <stdarg.h>

/* Number of elements in a stack-allocated array (compile-time only). */
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* Logging helpers */
#define LOG_ERROR(msg, fmt, ...) Logger("ERROR", __func__, __LINE__, msg, fmt, ##__VA_ARGS__)
#define LOG_INFO(msg, fmt, ...)  Logger("INFO",  __func__, __LINE__, msg, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(msg, fmt, ...) Logger("DEBUG", __func__, __LINE__, msg, fmt, ##__VA_ARGS__)

#define SET_TIMEOUT(tv, ms)            \
    do {                               \
        (tv)->tv_sec  = (ms) / 1000;    \
        (tv)->tv_usec = ((ms) % 1000) * 1000; \
    } while (0)

typedef struct dynamic_array
{
	char* pointer;      /* Data buffer */
	char* temp_pointer; /* Temporary pointer used during realloc */
	int len;            /* Number of used bytes */
	int size;           /* Allocated bytes */
}  dynamic_array;

struct connection_t
{
	socket_t sock;
	struct sockaddr_storage addr;
	int addrlen;
	char* address;
	char* port;
	dynamic_array* buffer;
};

/* Global API initialization state */

static char api_init = 0;

#ifdef _WIN32
static WSADATA wsa;
#endif

/* Internal parse symbols used in the flat binary representation produced by ParseJson. */
typedef enum ParseSymbol
{
    START_OF_KEY         = 2,    /* STX — marks start of a key token     */
    END_OF_VALUE         = 0,    /* NUL — marks end of a value token      */
    END_OF_NESTED_NUMBER = '|',  /* separates nesting depth number         */
    KEY                  = 30    /* RS  — separates key name from value   */
} ParseSymbol;

static int Reconnect(connection_t* connection);
static void Logger(const char* type, const char* func, int line, const char* message, const char* variable, ...);
static int CreateSocket(connection_t* connection);
static int ConnectWithTimeout(connection_t* connection);
static int ParseHeader(char header[], int* json_len);
static int ParseJson(connection_t* connection);
static int ParseJsonNested(connection_t* connection, int* nested_number, int *inx, int* json_inx, const int end_inx);
static int RecvJson(connection_t* connection);
static int SendJson(connection_t* connection);
static int ReallocArray(connection_t* connection, int new_size);
static int CallocAray(connection_t* connection, int size);
static int PoolConnectionInitFunction(init_pool_worker_t* init_t, pool_worker_t* worker);
static void PoolConnectionDestroyFunc(pool_worker_t* worker);
static int PoolConnectionWorkerFunc(pool_worker_t* worker);
static int SearchInJson(dynamic_array* connection, char* search[], int search_len, char** result);

#define ATTEMPTS 3 /* max reconnect attempts per request */

static int Reconnect(connection_t* connection)
{
	if (connection == NULL)
	{
		LOG_ERROR("Reconnect failed", "connection argument is NULL");
		return INVALID_ARGUMENT;
	}
	//close old socket
	if (connection->sock != INVALID_SOCKET)
	{
		closesocket(connection->sock);
		connection->sock = INVALID_SOCKET;
	}
	//create new socket
	if (CreateSocket(connection)) return CREATE_SOCKET_ERROR;
	//connect with timeout
	if (ConnectWithTimeout(connection)) return CONN_TIMEOUT;
	
	return CONN_OK;
}

/* Write one log entry to file */
static void Logger(const char* type, const char* func, int line, const char* message, const char* variable, ...)
{
	if(LOG_RESTAPI == 0)
	{
		return;
	}

	if (!strncmp(type, "DEBUG", sizeof("DEBUG")) && !DEBUG_RESTAPI)
	{
		return;
	}

#ifdef _WIN32
	FILE* log_file = fopen("C:\\LOG\\restapi.log", "a+");
	if (log_file == NULL) log_file = fopen(".\\restapi.log", "a+");
#else
	FILE* log_file = fopen("./restapi.log", "a+");
#endif
	if (log_file == NULL) return;
	
	time_t now = time(NULL);
	struct tm* t = localtime(&now);
	char timestamp[32];
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", t);

	// write error in log file
	fprintf(log_file, "[%s] [%s] %s (line:%d): %s [", timestamp, type, func, line, message);
	va_list argv;
	va_start(argv, variable);
	vfprintf(log_file, variable, argv);
	va_end(argv);
	fprintf(log_file, "]\n");
	fclose(log_file);

	return;
}

static int hexval(char c)
{
    if (c <= '9') return c - '0';
    if (c <= 'F') return c - 'A' + 10;
    return c - 'a' + 10;
}

static int utf8_encode(char *out, uint32_t code)
{
    if (code < 0x80)
    {
        out[0] = code;
        return 1;
    }
    else if (code < 0x800)
    {
        out[0] = 0xC0 | (code >> 6);
        out[1] = 0x80 | (code & 0x3F);
        return 2;
    }
    else
    {
        out[0] = 0xE0 | (code >> 12);
        out[1] = 0x80 | ((code >> 6) & 0x3F);
        out[2] = 0x80 | (code & 0x3F);
        return 3;
    }
}

int InitApi()
{
	#ifdef _WIN32
	if (api_init == 0)
	{
		int error_code = WSAStartup(MAKEWORD(2,2), &(wsa));
		if (error_code != 0)
		{
			LOG_ERROR("WSAStartup failed", "error code: %d", error_code);
			return WSA_STARTUP_ERROR;
		}
	}
	#endif
	api_init = 1;
	return 0;
}

connection_t* CreateConnection(char* address, char* port)
{

	if (api_init == 0)
	{
		LOG_ERROR("CreateConnection failed", "API is not initialized");
		return NULL;
	}

	if (address == NULL)
	{
		LOG_ERROR("CreateConnection failed", "address argument is NULL");
		return NULL;
	}
	if (port == NULL)
	{
		LOG_ERROR("CreateConnection failed", "port argument is NULL");
		return NULL;
	}

	connection_t *connection = (connection_t*)malloc(sizeof(connection_t));

	if (connection == NULL)
	{
		LOG_ERROR("CreateConnection failed", "memory allocation for connection failed");
		return NULL;
	}

	dynamic_array *buffer = (dynamic_array*)malloc(sizeof(dynamic_array));

	if (buffer == NULL)
	{
		LOG_ERROR("CreateConnection failed", "memory allocation for connection buffer failed");
		free(connection);
		return NULL;
	}

	buffer->len=0;
	buffer->size = 0;
	buffer->pointer = NULL;

	connection->address = address;
	connection->port = port;
	connection->sock = INVALID_SOCKET;
	connection->buffer = buffer;

	if ((CreateSocket(connection) != CONN_OK) || (ConnectWithTimeout(connection) != CONN_OK))
	{
		LOG_ERROR("CreateConnection failed", "socket creation or connect failed");
		if (connection->sock != INVALID_SOCKET)
		{
			closesocket(connection->sock);
			connection->sock = INVALID_SOCKET;
		}
		free(buffer);
		free(connection);
		return NULL;
	}

	if (CallocAray(connection, 10) != 0)
	{
		LOG_ERROR("CreateConnection failed", "failed to allocate initial request buffer");
		if (connection->sock != INVALID_SOCKET)
		{
			closesocket(connection->sock);
			connection->sock = INVALID_SOCKET;
		}
		free(buffer);
		free(connection);
		return NULL;
	}

	return connection;
}

void DestroyConnection(connection_t* connection)
{
	if(connection == NULL)
	{
		return;
	}
	if (connection->sock != INVALID_SOCKET)
	{
		closesocket(connection->sock);
		connection->sock = INVALID_SOCKET;
	}
	if (connection->buffer != NULL)
	{
		if (connection->buffer->pointer != NULL)
		{
			free(connection->buffer->pointer);
		}
		free(connection->buffer);
	}
	free(connection);

	connection = NULL;

	return;
}

/* Create socket and configure options */
static int CreateSocket(connection_t* connection)
{

	if (connection == NULL)
	{
		LOG_ERROR("CreateSocket failed", "connection argument is NULL");
		return INVALID_ARGUMENT;
	}
	if (connection->address == NULL)
	{
		LOG_ERROR("CreateSocket failed", "connection address is NULL");
		return INVALID_ARGUMENT;
	}
	if (connection->port == NULL)
	{
		LOG_ERROR("CreateSocket failed", "connection port is NULL");
		return INVALID_ARGUMENT;
	}
	#ifdef _WIN32
	if (api_init == 0)
	{
		LOG_ERROR("CreateSocket failed", "API is not initialized");
		return INIT_ERROR;
	}
	#endif

	//skip on linux
	

	struct addrinfo addres_search_parameter;
	struct addrinfo* addres_search_result = NULL;
	struct addrinfo* addres;
	
	//close original socket if exist
	if (connection->sock != INVALID_SOCKET)
	{
		closesocket(connection->sock);
		connection->sock = INVALID_SOCKET;
	}
	
	//define server info
	memset(&addres_search_parameter, 0, sizeof(addres_search_parameter)); // Fill the entire hints structure with zeros before use
	addres_search_parameter.ai_family = AF_UNSPEC;       // IPv4 and IPv6
	addres_search_parameter.ai_socktype = SOCK_STREAM;
	addres_search_parameter.ai_protocol = IPPROTO_TCP;

	//get info about server
	int status = getaddrinfo(connection->address, connection->port, &addres_search_parameter, &addres_search_result); //return pointer on struct of addrinfo
	if (status != 0)
	{
		LOG_ERROR("getaddrinfo failed", "host=%s, port=%s, err=%s", connection->address, connection->port, gai_strerrorA(status));
		return CONN_SETUP_ERROR;
	}

	//list all struct adress [if include ai_next, server have more connect possibility]
	for (addres = addres_search_result; addres != NULL; addres = addres->ai_next)
	{
		connection->sock = socket(addres->ai_family, addres->ai_socktype, addres->ai_protocol);
		if (connection->sock == INVALID_SOCKET)
		{
			LOG_DEBUG("socket creation failed", "host=%s, port=%s, family=%d, err=%d",connection->address,connection->port,addres->ai_family,WSAGetLastError());
			continue;
		}

		memcpy(&connection->addr, addres->ai_addr, addres->ai_addrlen);
		connection->addrlen = addres->ai_addrlen;

		#ifdef _WIN32
		DWORD timeout_dw = RESTAPI_COMM_TIMEOUT_MS;
		int timeout_sock_len = (int)sizeof(timeout_dw);
		const char* timeout_sock = (const char*)&timeout_dw;
		#else
		struct timeval tv;
		SET_TIMEOUT(&tv, RESTAPI_COMM_TIMEOUT_MS);
		int timeout_sock_len = (int)sizeof(tv);
		const struct timeval* timeout_sock = &tv;
		#endif

		//recieve timeout
		if (setsockopt(connection->sock, SOL_SOCKET, SO_RCVTIMEO, timeout_sock, timeout_sock_len) == SOCKET_ERROR)
		{
			LOG_ERROR("setsockopt SO_RCVTIMEO failed", "");
			closesocket(connection->sock);
			connection->sock = INVALID_SOCKET;
			freeaddrinfo(addres_search_result);
			return CONN_SET_TIMEOUT_ERROR;
		}

		//send_timeout
		if (setsockopt(connection->sock, SOL_SOCKET, SO_SNDTIMEO, timeout_sock, timeout_sock_len) == SOCKET_ERROR)
		{
			LOG_ERROR("setsockopt SO_SNDTIMEO failed", "");
			closesocket(connection->sock);
			connection->sock = INVALID_SOCKET;
			freeaddrinfo(addres_search_result);
			return CONN_SET_TIMEOUT_ERROR;
		}

		freeaddrinfo(addres_search_result);
		return CONN_OK;
	}

	//dont find avaliable addres
	freeaddrinfo(addres_search_result);

	LOG_ERROR("no suitable address found", "host=%s, port=%s", connection->address, connection->port);
	return CONN_ERROR;
}

//Create connection with handle timeout
static int ConnectWithTimeout(connection_t* connection)
{
	#ifdef _WIN32
	u_long mode = 1;
	#else
	unsigned long mode = 1;
	#endif

	if (connection == NULL || connection->sock == INVALID_SOCKET || connection->address == NULL || connection->port == NULL)
	{
		LOG_ERROR("ConnectWithTimeout failed", "invalid connection argument or socket");
		return INVALID_ARGUMENT;
	}

	struct sockaddr* addr = (struct sockaddr*)&(connection->addr);
	char ip[INET6_ADDRSTRLEN] = {0};
    unsigned int port = 0;

	//switch between ipv4 a ipv6
    if (addr->sa_family == AF_INET)
    {
        struct sockaddr_in* in4 = (struct sockaddr_in*)addr;
        if (getnameinfo((const struct sockaddr*)in4, sizeof(*in4), ip, sizeof(ip), NULL, 0, NI_NUMERICHOST) != 0)
		{
			LOG_ERROR("getnameinfo failed", "");
        	return CONN_SETUP_ERROR;
		}
        port = ntohs(in4->sin_port);
    }
    else if (addr->sa_family == AF_INET6)
    {
        struct sockaddr_in6* in6 = (struct sockaddr_in6*)addr;
        if (getnameinfo((const struct sockaddr*)in6, sizeof(*in6), ip, sizeof(ip), NULL, 0, NI_NUMERICHOST) != 0)
        {
			LOG_ERROR("getnameinfo failed", "");
        	return CONN_SETUP_ERROR;
		}
		port = ntohs(in6->sin6_port);
    }
    else
    {
        LOG_ERROR("unsupported address family", "addr=unknown family=%d", addr->sa_family);
        return CONN_SETUP_ERROR;
    }

	//set no waiting mode for answer
	if (ioctlsocket(connection->sock, FIONBIO, &mode) != 0)
	{
		LOG_ERROR("ioctlsocket failed", "");
		return CONN_SETUP_ERROR;
	}

	//try connect
	if (connect(connection->sock, addr, connection->addrlen) == 0)
	{
		// connection immediately success
		goto ok_switch_mode;
	}
	else
	{
		int err = WSAGetLastError();
		#ifdef _WIN32
		if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS && err != WSAEINVAL && err != WSAEALREADY)
		{
		#else
		if (err != EINPROGRESS && err != EALREADY && err != EWOULDBLOCK && err != EAGAIN)
		{
		#endif
			//connect failed
			
			LOG_ERROR("connect failed", "");
			goto error_switch_mode;
		}
	}

	// wait for connect to server
	fd_set writefds;
	FD_ZERO(&writefds);
	FD_SET(connection->sock, &writefds);

	#ifdef _WIN32
	int nfds = 0;
	#else
	int nfds = (int)connection->sock + 1;
	#endif
	
	struct timeval tv;
	SET_TIMEOUT(&tv, RESTAPI_COMM_TIMEOUT_MS);
	/* wait until socket is writable or timeout expires */
	int socket_handles_count = select(nfds, NULL, &writefds, NULL, &tv);

	if (socket_handles_count > 0)
	{
		//check if connect is really made
		int err = 0;
		#ifdef _WIN32
		int len = (int)sizeof(err);
		#else
		socklen_t len = (socklen_t)sizeof(err);
		#endif

		if (getsockopt(connection->sock, SOL_SOCKET, SO_ERROR, (char*)&err, &len) != 0)
		{
			LOG_ERROR("connect failed because ioctlsocket", "");
			goto error_switch_mode;
		}

		if (err == 0)
		{
			goto ok_switch_mode;
		}
		else
		{
			WSASetLastError(err);
			LOG_ERROR("connection attempt failed", "addr=%s:%d, error=%d", ip, port, err);
			goto error_switch_mode;
		}
	}
	else
	{
		// timeout or select error

		if (socket_handles_count == 0)
		{
			LOG_ERROR("timeout error", "addr=%s:%d, error=%d",ip, port, WSAGetLastError());
		}
		else
		{
			LOG_ERROR("select failed", "addr=%s:%d, error=%d",ip, port, WSAGetLastError());
		}

		goto error_switch_mode;
	}

	error_switch_mode:
	mode = 0;
	if( ioctlsocket(connection->sock, FIONBIO, &mode) != 0)
	{
		LOG_ERROR("connect failed because ioctlsocket", "");
		return CONN_ERROR;
	}
	return CONN_ERROR;
	
	ok_switch_mode:
	mode = 0;
	if( ioctlsocket(connection->sock, FIONBIO, &mode) != 0)
	{
		LOG_ERROR("connect failed because ioctlsocket", "");
		return CONN_ERROR;
	}
	return CONN_OK;
}

static int ParseHeader(char header[], int* json_len)
{
	if (header == NULL)
	{
		LOG_ERROR("ParseHeader invalid input", "header is NULL");
		return INVALID_ARGUMENT;
	}

	if (json_len != NULL)
	{
		*json_len = -1;
	}

    int inx = 0, inx_json_len = 0;
    short code = -1;
    char change = 0;

    //skip to HTTP 
    while (header[inx] < 33)
    {
        if (header[inx] == '\0') goto end_of_header; 
        inx++;
    }

    //skip to code
    while (header[inx] != ' ')
    {
        if (header[inx] == '\0') goto end_of_header; 
        inx++;
    }

    //skip space before code
    inx++;
    if (header[inx] == '\0') goto end_of_header;

    //place \0 3 symbol away
    for (int limit = inx + 3; inx < limit; inx++)
    {
        if (header[inx] == '\0') goto end_of_header;
    }
    
    change = header[inx];
    header[inx] = '\0';

    //store code
    if (sscanf(header + (inx - 3), "%hd", &code) != 1)
    {
        LOG_ERROR("ParseHeader read code error", "failed to read response code at inx=%d", inx);
        return READ_CODE_ERROR;
    }
    header[inx] = change;
    inx++;

    //check return code from server
    if (code != 200 && code != 201 && code != 202)
    {
        LOG_ERROR("ParseHeader invalid response code", "invalid response code=%hd", code);
        return INVALID_CODE;
    }

    //search for length
	if (json_len == NULL)
	{
		return PARSE_OK;
	}

    char search[] = "content-length";
    short search_len = ARRAY_LEN(search);
    short search_inx = 0;

    while(1)
    {
        if (header[inx] == '\0') goto end_of_header;

        if(search[search_inx] == header[inx] || search[search_inx] - ('a' - 'A') == header[inx])
        {
            search_inx++;
            if(search_inx == search_len - 1)
            {
                inx++;
                break;
            }
        }
        else
        {
            search_inx = 0;
        }
        inx++;
    }

    //read white until :
    while (header[inx] != ':')
    {
        if (header[inx] == '\0') goto end_of_header;
        inx++;
    }
    inx++;

    //read all white symbol before len start
    while (header[inx] < 33)
    {
        if (header[inx] == '\0') goto end_of_header;
        inx++;
    }

    inx_json_len = inx;

    while(header[inx] > ' ')
    {
        inx++;
    }

    if (header[inx] == '\0') goto end_of_header;

    change = header[inx];
    header[inx] = '\0';

    if (sscanf(header + (inx_json_len), "%d", json_len) != 1)
    {
        LOG_ERROR("ParseHeader read length error", "failed to read content-length at inx=%d", inx_json_len);
        return READ_LENGTH_ERROR;
    }

    header[inx] = change;

    return PARSE_OK;


    end_of_header:
    if (code == -1)
    {
        LOG_ERROR("ParseHeader code not found", "");
        return CODE_NOT_FOUND;
    }
	else if (json_len != NULL && *json_len == -1)
    {
        *json_len = 0;
		return PARSE_OK;
    }
    LOG_ERROR("ParseHeader unknown error", "");
    return UNKNOWN_ERROR;
}

static int ParseJson(connection_t* connection)
{
	int nested_number = 1;
    int inx = 0;
    int json_inx = 0;
	int status = -1;

	if (connection == NULL || connection->buffer == NULL || connection->buffer->pointer == NULL)
	{
		LOG_ERROR("ParseJson failed", "invalid connection or JSON buffer");
		return INVALID_ARGUMENT;
	}

	const int end_inx = connection->buffer->len;

	status = ParseJsonNested(connection, &nested_number, &inx, &json_inx, end_inx);

	if (status != PARSE_OK)
	{
		LOG_ERROR("ParseJson failed", "nested parser returned status=%d", status);
		return status;
	}

	connection->buffer->len = json_inx;

	memcpy(connection->buffer->pointer, connection->buffer->pointer + end_inx, json_inx);

	connection->buffer->pointer[json_inx] = -1;
	
	return PARSE_OK;
}

static int ParseJsonNested(connection_t* connection, int* nested_number, int *inx, int* json_inx, const int end_inx)
{
	if (connection == NULL || connection->buffer == NULL || connection->buffer->pointer == NULL ||
		nested_number == NULL || inx == NULL || json_inx == NULL || end_inx < 0 ||
		connection->buffer->size < end_inx)
	{
		LOG_ERROR("ParseJsonNested failed", "invalid arguments or buffer state");
		return INVALID_ARGUMENT;
	}


    int mode = 0, escape = 0, read_word = 0, value_type = 0;
    char list_in_string = 0;
    char list_started = 0;
	char unicode[4] = {0};
    int list_depth = 0;

	unsigned char* parse_json_ptr = (unsigned char*)connection->buffer->pointer+end_inx;
	int* size = &connection->buffer->size;
	unsigned char* origo_json_ptr = (unsigned char*)connection->buffer->pointer;

    while(++(*inx) < end_inx)
    {
        if ((*json_inx) + 10 >= *size - end_inx && ReallocArray(connection, ( (*json_inx) + end_inx + 10) * 2) != 0) //+10 for safe copy like unicode with max 3 symbol
        {
            return MEMORY_ERROR;
        }
        //if needed realloc json
        switch(mode)
        {
            case 0:

				if (read_word == 0 && escape == 0 && origo_json_ptr[(*inx)] == '}')
				{
					(*json_inx) += snprintf(parse_json_ptr + (*json_inx), (*size) - end_inx - (*json_inx), "%c%d%c", END_OF_VALUE, (*nested_number)-1, END_OF_NESTED_NUMBER);
					mode = 3;
					(*inx)--;
					continue;
				}

                if (escape == 0)
                {
                    if (origo_json_ptr[(*inx)] == '\\')
                    {
                        escape = 1;
                        continue;
                    }
                    if (origo_json_ptr[(*inx)] == '"')
                    {
                        if (read_word == 0)
                        {
                            read_word = 1;
                            if ((*size) - end_inx < (*json_inx) + 10 && ReallocArray(connection, ((*json_inx) + end_inx + 10) * 2) != 0)
                            {
                                //not enought space for start
								return MEMORY_ERROR;
                            }

                            (*json_inx) += snprintf(parse_json_ptr + (*json_inx), (*size) - end_inx - (*json_inx), "%c%d%c", START_OF_KEY, *nested_number, END_OF_NESTED_NUMBER);
                            if ((*size) - end_inx <= (*json_inx))
                            {
                                //heap buffer overflow
                                return MEMORY_ERROR; 
                            }

                            continue;
                        }
                        else if (read_word != 0)
                        {
                            parse_json_ptr[(*json_inx)++] = KEY;
                            mode = 1;
                            read_word = 0;
                            continue;
                        }
                    }
                }

                if (read_word)
                {
                    if (escape == 1)
                    {
                        switch(origo_json_ptr[(*inx)])
                        {
                            case '"':
                                parse_json_ptr[(*json_inx)++] = '"';
                                break;
                            case '\\':
                                parse_json_ptr[(*json_inx)++] = '\\';
                                break;
                            case '/':
                                parse_json_ptr[(*json_inx)++] = '/';
                                break;
                            case 'b':
                                parse_json_ptr[(*json_inx)++] = '\b';
                                break;
                            case 'f':
                                parse_json_ptr[(*json_inx)++] = '\f';
                                break;
                            case 'n':
                                parse_json_ptr[(*json_inx)++] = '\n';
                                break;
                            case 'r':
                                parse_json_ptr[(*json_inx)++] = '\r';
                                break;
                            case 't':
                                parse_json_ptr[(*json_inx)++] = '\t';
                                break;
                            case 'u':
                                if (4 + (*inx) >= end_inx)
									{
										LOG_ERROR("ParseJson invalid json", "invalid unicode escape in list inx=%d json_inx=%d", *inx, *json_inx);
										return INVALID_JSON;
									}
									
									(*json_inx)++;

									uint32_t code =
										(hexval(origo_json_ptr[++(*inx)]) << 12) |
										(hexval(origo_json_ptr[++(*inx)]) << 8)  |
										(hexval(origo_json_ptr[++(*inx)]) << 4)  |
										hexval(origo_json_ptr[++(*inx)]);

									int len = utf8_encode(unicode, code);

									for (int i = 0; i < len; i++)
									{
										parse_json_ptr[(*json_inx)++] = unicode[i];
										printf("unicode char: %02x\n", (unsigned char)unicode[i]);
									}
									break;
                            default:
                                LOG_ERROR("ParseJson invalid escape", "invalid escape=\\%c inx=%d json_inx=%d", origo_json_ptr[(*inx)], *inx, *json_inx);
                                return PARSE_ERROR;
                        }
                        escape = 0;
                        continue;
                    }
                    parse_json_ptr[(*json_inx)++] = origo_json_ptr[(*inx)];
                }

                break;
            case 1:
                if(origo_json_ptr[(*inx)] == ':')
                {
                    mode++;
                    (*inx)++;
                    while (origo_json_ptr[(*inx)] != '\0' && origo_json_ptr[(*inx)] < 33)
                    {
                        (*inx)++;
                    }
                    if (origo_json_ptr[(*inx)] == '\0')
                    {
                        LOG_ERROR("ParseJson invalid json", "unexpected end after ':' inx=%d json_inx=%d", *inx, *json_inx);
                        return INVALID_JSON;
                    }
                    (*inx)--;
                }
                continue;
            case 2:
                if (value_type == 0 && origo_json_ptr[(*inx)] == '{')
                {

                    (*nested_number)++;
                    if (ParseJsonNested(connection, nested_number, inx, json_inx, end_inx) != PARSE_OK)
                    {
                        LOG_ERROR("ParseJson nested parse error", "nested parse failed inx=%d json_inx=%d nested=%d", *inx, *json_inx, *nested_number);
                        return PARSE_ERROR;
                    }
					if(origo_json_ptr[(*inx)] == '\0') {return INVALID_JSON;}
                    (*nested_number)--;
                    mode = 3;
                    continue;
                }

                // string with quotes
                if (value_type == 0)
                {
                    if (origo_json_ptr[(*inx)] == '"')
                    {
                        value_type = 1;
                        continue;
                    }
                    else if (origo_json_ptr[(*inx)] == '[')
                    {
                        value_type = 2;
                    }
                    else
                    {
                        value_type = 3;
                    }
                }

                if (value_type == 1)
                {
                    if (escape == 0)
                    {
                        if (origo_json_ptr[(*inx)] == '\\') { escape = 1; continue; }
                        if (origo_json_ptr[(*inx)] == '"')
                        {
                            if ((*size) - end_inx < (*json_inx) + 10 && ReallocArray(connection, ((*json_inx) + end_inx + 10) * 2) != 0)
                            {
                                //not enought space for start
                                return MEMORY_ERROR;
                            }

                            (*json_inx) += snprintf(parse_json_ptr + (*json_inx), (*size) - end_inx - (*json_inx), "%c%d%c", END_OF_VALUE, *nested_number, END_OF_NESTED_NUMBER);
                            if ((*size) - end_inx <= (*json_inx))
                            {
                                //heap buffer overflow
                                return MEMORY_ERROR; 
                            }
                            mode = 3;
                            continue;
                        }
                        if (origo_json_ptr[(*inx)] < ' ')
                        {
                            LOG_ERROR("ParseJson parse error", "control char in string inx=%d json_inx=%d", *inx, *json_inx);
                            return PARSE_ERROR;
                        }
                    }
                    else
                    {
                        switch (origo_json_ptr[(*inx)])
                        {
                            case '"':  parse_json_ptr[(*json_inx)++] = '"';  break;
                            case '\\': parse_json_ptr[(*json_inx)++] = '\\'; break;
                            case '/':  parse_json_ptr[(*json_inx)++] = '/';  break;
                            case 'b':  parse_json_ptr[(*json_inx)++] = '\b'; break;
                            case 'f':  parse_json_ptr[(*json_inx)++] = '\f'; break;
                            case 'n':  parse_json_ptr[(*json_inx)++] = '\n'; break;
                            case 'r':  parse_json_ptr[(*json_inx)++] = '\r'; break;
                            case 't':  parse_json_ptr[(*json_inx)++] = '\t'; break;
                            case 'u':
                                if (4 + (*inx) >= end_inx)
									{
										LOG_ERROR("ParseJson invalid json", "invalid unicode escape in list inx=%d json_inx=%d", *inx, *json_inx);
										return INVALID_JSON;
									}
									
									(*json_inx)++;

									uint32_t code =
										(hexval(origo_json_ptr[++(*inx)]) << 12) |
										(hexval(origo_json_ptr[++(*inx)]) << 8)  |
										(hexval(origo_json_ptr[++(*inx)]) << 4)  |
										hexval(origo_json_ptr[++(*inx)]);
										

									int len = utf8_encode(unicode, code);

									for (int i = 0; i < len; i++)
									{
										parse_json_ptr[(*json_inx)++] = unicode[i];
										printf("unicode char: %02x\n", (unsigned char)unicode[i]);
									}
									break;
                            default:
                                LOG_ERROR("ParseJson invalid json", "invalid escape=\\%c inx=%d json_inx=%d", origo_json_ptr[(*inx)], *inx, *json_inx);
                                return INVALID_JSON;
                        }
                        escape = 0;
                        continue;
                    }

                    parse_json_ptr[(*json_inx)++] = origo_json_ptr[(*inx)];
                    continue;
                }

                else if (value_type == 2)
                {
                    if (list_started == 0)
                    {
                        if (origo_json_ptr[(*inx)] != '[')
                        {
                            LOG_ERROR("ParseJson invalid json", "list start missing inx=%d json_inx=%d", *inx, *json_inx);
                            return INVALID_JSON;
                        }
                        list_started = 1;
                        list_in_string = 0;
                        list_depth = 1;
                        escape = 0;
                        parse_json_ptr[(*json_inx)++] = '[';
                        continue;
                    }

                    if (list_in_string)
                    {
                        if (escape == 0)
                        {
                            if (origo_json_ptr[(*inx)] == '\\') { escape = 1; continue; }
                            if (origo_json_ptr[(*inx)] == '"') { list_in_string = 0; }
                            if (origo_json_ptr[(*inx)] < ' ')
                            {
                                LOG_ERROR("ParseJson invalid json", "control char in list string inx=%d json_inx=%d, symbol=%d", *inx, *json_inx, (int)origo_json_ptr[(*inx)]);
                                return INVALID_JSON;
                            }
                        }
                        else
                        {
                            switch (origo_json_ptr[(*inx)])
                            {
                                case '"':  parse_json_ptr[(*json_inx)++] = '"';  break;
                                case '\\': parse_json_ptr[(*json_inx)++] = '\\'; break;
                                case '/':  parse_json_ptr[(*json_inx)++] = '/';  break;
                                case 'b':  parse_json_ptr[(*json_inx)++] = '\b'; break;
                                case 'f':  parse_json_ptr[(*json_inx)++] = '\f'; break;
                                case 'n':  parse_json_ptr[(*json_inx)++] = '\n'; break;
                                case 'r':  parse_json_ptr[(*json_inx)++] = '\r'; break;
                                case 't':  parse_json_ptr[(*json_inx)++] = '\t'; break;
                                case 'u':
									if (4 + (*inx) >= end_inx)
									{
										LOG_ERROR("ParseJson invalid json", "invalid unicode escape in list inx=%d json_inx=%d", *inx, *json_inx);
										return INVALID_JSON;
									}
									
									(*json_inx)++;

									uint32_t code =
										(hexval(origo_json_ptr[++(*inx)]) << 12) |
										(hexval(origo_json_ptr[++(*inx)]) << 8)  |
										(hexval(origo_json_ptr[++(*inx)]) << 4)  |
										hexval(origo_json_ptr[++(*inx)]);

									int len = utf8_encode(unicode, code);

									for (int i = 0; i < len; i++)
									{
										parse_json_ptr[(*json_inx)++] = unicode[i];
										printf("unicode char: %02x\n", (unsigned char)unicode[i]);
									}
									break;
									
                                default:
                                    LOG_ERROR("ParseJson invalid json", "invalid escape in list inx=%d json_inx=%d, symbol=%d", *inx, *json_inx, (int)origo_json_ptr[(*inx)]);
                                    return INVALID_JSON;
                            }
                            escape = 0;
                            continue;
                        }
                    }
                    else
                    {
                        if (origo_json_ptr[(*inx)] == '"') { list_in_string = 1; }
                        else if (origo_json_ptr[(*inx)] == '[') { list_depth++; }
                        else if (origo_json_ptr[(*inx)] == ']')
                        {
                            list_depth--;
                            if (list_depth == 0)
                            {
                                if ((*size) - end_inx < (*json_inx) + 10 && ReallocArray(connection, ((*json_inx) + end_inx + 10) * 2) != 0)
                                {
                                    //not enought space for start
                                    return MEMORY_ERROR;
                                }

                                (*json_inx) += snprintf(parse_json_ptr + (*json_inx), (*size) - end_inx - (*json_inx), "]%c%d%c", END_OF_VALUE, *nested_number, END_OF_NESTED_NUMBER);
                                if ((*size) - end_inx <= (*json_inx))
                                {
                                    //heap buffer overflow
                                    return MEMORY_ERROR; 
                                }
                                mode = 3;
                                value_type = 0;
                                list_started = 0;
                                continue;
                            }
                        }
                        else if ((origo_json_ptr[(*inx)] == ' ' || origo_json_ptr[(*inx)] == '\t' || origo_json_ptr[(*inx)] == '\n' || origo_json_ptr[(*inx)] == '\r'))
                        {
                            continue;
                        }
                    }

                    parse_json_ptr[(*json_inx)++] = origo_json_ptr[(*inx)];
                }

                else if (value_type == 3 || value_type == 4)
                {
                    if (origo_json_ptr[(*inx)] < ' ' || origo_json_ptr[(*inx)] == '}' || origo_json_ptr[(*inx)] == ',')
                    {
                        if (value_type == 3)
                        {
                            LOG_ERROR("ParseJson invalid json", "empty value inx=%d json_inx=%d", *inx, *json_inx);
                            return INVALID_JSON; //handle : } nebo : , value_type = 3 je prvni znak
                        }
                        if ((*size) - end_inx < (*json_inx) + 10 && ReallocArray(connection, ((*json_inx) + end_inx + 10) * 2) != 0)
                        {
                            //not enought space for start
                            return MEMORY_ERROR;
                        }

                        (*json_inx) += snprintf(parse_json_ptr + (*json_inx), (*size) - end_inx - (*json_inx), "%c%d%c", END_OF_VALUE, *nested_number, END_OF_NESTED_NUMBER);
                        if ((*size) - end_inx <= (*json_inx))
                        {
                            //heap buffer overflow
                            return MEMORY_ERROR; 
                        }
                        (*inx) --;
                        mode = 3;
                        continue;
                    }
                    value_type = 4;
                    parse_json_ptr[(*json_inx)++] = origo_json_ptr[(*inx)];
                }
                continue;
            case 3:
                value_type = 0;
                while (origo_json_ptr[(*inx)] != '\0')
                {
                    if (origo_json_ptr[(*inx)] == '}')
                    {
                        parse_json_ptr[(*json_inx)] = '\0';
                        return PARSE_OK;
                    }
                    if(origo_json_ptr[(*inx)] == ',')
                    {
                        mode = 0;
                        break;
                    }
                    (*inx)++;
                }
                if (origo_json_ptr[(*inx)] == '\0')
                {
                    LOG_ERROR("ParseJson invalid json", "unexpected end after value inx=%d json_inx=%d", *inx, *json_inx);
                    return INVALID_JSON;
                }
                continue;
                
        }   
    }

    LOG_ERROR("ParseJson parse error", "unexpected end of json inx=%d json_inx=%d", *inx, *json_inx);
    return PARSE_ERROR;
}

int SearchInTask(void* task_v, char *search[], int search_len, char** result)
{
	task_t* task = (task_t*)task_v;
	if (task == NULL)
	{
		LOG_ERROR("SearchInTask failed", "invalid argument");
		return -1;
	}

	return SearchInJson(task->buffer, search, search_len, result);
}

int SearchInConnection(connection_t* conn, char *search[], int search_len, char** result)
{
	if (conn == NULL)
	{
		LOG_ERROR("SearchInTask failed", "invalid argument");
		return -1;
	}

	return SearchInJson(conn->buffer, search, search_len, result);
}

static int SearchInJson(dynamic_array* buffer, char *search[], int search_len, char** result)
{
    int nested_json = 1;
    int json_inx = -1;
    int search_item = 0, nested_number_in_text = 0;
    int search_inx = 0;

	if (buffer == NULL || buffer->pointer == NULL || search == NULL || result == NULL || search_len <= 0)
	{
		LOG_ERROR("SearchInJson failed", "invalid input argument");
		return INVALID_ARGUMENT;
	}

	for (int i = 0; i < search_len; i++)
	{
		if (search[i] == NULL)
		{
			LOG_ERROR("SearchInJson failed", "search[%d] is NULL", i);
			return INVALID_ARGUMENT;
		}
	}

	*result = NULL;

	char* parse_json_ptr = buffer->pointer;

    char mode = 0;

    while(++json_inx < buffer->len)
    {
        if(search_item >= search_len)
        {
            return VALUE_NOT_FOUND;
        }

        switch (mode)
        {
        case 0:
            //search nested start
            if (parse_json_ptr[json_inx] != START_OF_KEY) continue;
            json_inx++;
            if (sscanf(parse_json_ptr + json_inx, "%d", &nested_number_in_text) != 1) return VALUE_NOT_FOUND;
            if (nested_json > nested_number_in_text) return VALUE_NOT_FOUND; //handle multiple nested json
            else if (nested_json != nested_number_in_text) continue;
            mode++;
            search_inx = 0;
            continue;
        case 1:
            if (search[search_item][search_inx] == '\0')
            {
                if (search_item == search_len - 1)
                {
                    //read key
                    while (parse_json_ptr[json_inx] != KEY)
                    {
                        if (json_inx >= buffer->len) return VALUE_NOT_FOUND;
                        json_inx++;
                    }
                    json_inx++;
                    *result = parse_json_ptr + json_inx;

                    return 0;
                }
                else
                {
                    //nested json
                    search_item++;
                    nested_json++;
                    mode = 0;
                }
            }

            if (search[search_item][search_inx] == parse_json_ptr[json_inx])
            {
                search_inx++;
                continue;
            }
            else
            {
                search_inx = 0;
            }

        default:
            break;
        }


    }

    return VALUE_NOT_FOUND;
}

static int ReallocArray(connection_t* connection, int new_size)
{
	if (connection == NULL || connection->buffer == NULL || connection->buffer->pointer == NULL || new_size <= 0)
	{
		LOG_ERROR("ReallocArray failed", "invalid connection or buffer");
		return INVALID_ARGUMENT;
	}

	connection->buffer->temp_pointer = (char*) realloc(connection->buffer->pointer, new_size * sizeof(char));
	if(connection->buffer->temp_pointer == NULL)
	{
		free(connection->buffer->pointer);
		connection->buffer->pointer = NULL;
		LOG_ERROR("failed to realloc memory for request", "realoc size: %d",new_size);
		return MEMORY_ERROR;
	}
	
	connection->buffer->pointer = connection->buffer->temp_pointer;
	if (new_size > connection->buffer->size)
	{
		memset(connection->buffer->pointer + connection->buffer->size, 0, new_size - connection->buffer->size);
	}
	connection->buffer->size = new_size;

	return 0;
}

static int CallocAray(connection_t* connection, int size)
{
	if (connection == NULL || connection->buffer == NULL || size <= 0)
	{
		LOG_ERROR("CallocAray failed", "invalid connection or buffer");
		return INVALID_ARGUMENT;
	}

	if (connection->buffer->pointer != NULL)
	{
		free(connection->buffer->pointer);
	}

	connection->buffer->pointer = (char*)calloc(size, sizeof(char));
	if (connection->buffer->pointer == NULL)
	{
		LOG_ERROR("failed to realloc memory for request", "realoc size: %d",size);
		return MEMORY_ERROR;
	}
	connection->buffer->size = size;
	connection->buffer->len = 0;

	return 0;
}

static int RecvJson(connection_t* connection)
{
	int total_received = 0;
	int received;
	int header_size = 0;
	int count_socket_handles = 0;
	int json_len = -1;

	if (connection == NULL || connection->sock == INVALID_SOCKET || connection->buffer == NULL)
	{
		LOG_ERROR("RecvJson failed", "invalid connection argument or socket");
		return INVALID_ARGUMENT;
	}

	#ifdef _WIN32
	int nfds = 0;
	#else
	int nfds = (int)connection->sock + 1;
	#endif

	// wait on socket is ready to read
	fd_set fds;

	//setup request memory
	if (connection->buffer->pointer == NULL && CallocAray(connection,16) != 0)
	{
		return MEMORY_ERROR;
	}
	else if (connection->buffer->size == 0 && ReallocArray(connection, 16) != 0)
	{
		return MEMORY_ERROR;
	}

	//null last data in request
	memset(connection->buffer->pointer, 0, connection->buffer->size);

	//recieve until read head
	recieve_head:

	//fds
	FD_ZERO(&fds);
	FD_SET(connection->sock, &fds);

	count_socket_handles = 0;
	/* wait for readable socket */
	struct timeval tv;
	SET_TIMEOUT(&tv, RESTAPI_COMM_TIMEOUT_MS);
	count_socket_handles = select(nfds, &fds, NULL, NULL, &tv);

	//timeout error
	if (count_socket_handles == 0)
	{
		LOG_ERROR("recv timeout while reading headers (select)", "");
		return CONN_TIMEOUT;
	}
	//error during wait
	else if (count_socket_handles < 0)
	{
		LOG_ERROR("recv error in select read", "error code: %d",WSAGetLastError());
		return CONN_ERROR;
	}

	//recieve data from socket with max size of request buffer
	received = recv(connection->sock, connection->buffer->pointer + total_received, connection->buffer->size - 1 - total_received, 0);

	if (received == SOCKET_ERROR)
	{
		int wsa_err = WSAGetLastError();
		LOG_ERROR("recv failed", "recv failed, WSAError=%d", wsa_err);
		return CONN_ERROR;
	}
		else if (received == 0)
		{
			LOG_DEBUG("recv json", "[received: %d, expected: %d]", received, connection->buffer->size - 1 - total_received);
			LOG_DEBUG("connection closed before headers complete", "");
			return CONN_NOT_CONNECTED;
		}

	total_received += received;

	if (total_received >= connection->buffer->size)
	{
		LOG_ERROR("rec_buffer overflow", "");
		return MEMORY_ERROR;
	}
	connection->buffer->pointer[total_received] = '\0';

	//search for \r\n\r\n
	for (int inx = 0, count_r = 0, count_n = 0; inx < total_received; inx++)
	{
		if(count_r == count_n && connection->buffer->pointer[inx] == '\r')
		{
			count_r++;
		}
		else if(count_r == count_n + 1 && connection->buffer->pointer[inx] == '\n')
		{
			count_n++;
		}
		else
		{
			count_n = 0;
			count_r = 0;
			continue;
		}

		if (count_n == 2 && count_r == 2)
		{
			connection->buffer->pointer[inx - 3] = '\0';
			header_size = inx + 1;

			goto parse_response; 
		}
	}

	if (total_received >= connection->buffer->size -2 && ReallocArray(connection, connection->buffer->size * 2) != 0)
	{
		return MEMORY_ERROR;
	}

	goto recieve_head;

	parse_response:
	//create space for parse_json_ptr
	if (header_size >= connection->buffer->size && ReallocArray(connection, connection->buffer->size * 2) != 0)
	{
		return MEMORY_ERROR;
	}

	if (ParseHeader(connection->buffer->pointer, &json_len) != PARSE_OK)
	{
		return PARSE_ERROR;
	}

	int want_recieve = json_len - (total_received - header_size);

	if (json_len == 0)
	{
		//no body
		return CONN_OK;
	}
	else if (want_recieve < 0)
	{
		LOG_ERROR("invalid receive length", "remaining body length is negative");
		return MEMORY_ERROR;
	}
	else if (want_recieve == 0)
	{
		if (total_received + 1 > connection->buffer->size && ReallocArray(connection, (total_received + 1) * 2) != 0)
		{
			return MEMORY_ERROR;
		}
		connection->buffer->pointer[total_received] = '\0';
	}
	else if(want_recieve > 0)
	{
		//recieve all
		//realloc enought for all
		if ((total_received + want_recieve) * 2 > connection->buffer->size && ReallocArray(connection, (total_received + want_recieve) * 2) != 0)
		{
			return MEMORY_ERROR;
		}

		received = recv(connection->sock, connection->buffer->pointer + total_received, connection->buffer->size - 1 - total_received, 0);

		if (received == SOCKET_ERROR)
		{
			int wsa_err = WSAGetLastError();
			LOG_ERROR("recv failed", "recv failed, WSAError=%d", wsa_err);
			return CONN_ERROR;
		}
		else if (received == 0)
		{
			LOG_INFO("connection closed before headers complete", "");
			return CONN_NOT_CONNECTED;
		}

		total_received += received;

		if (total_received >= connection->buffer->size)
		{
			LOG_ERROR("rec_buffer overflow", "");
			return MEMORY_ERROR;
		}

		connection->buffer->pointer[total_received] = '\0';


		if (total_received - header_size != json_len)
		{
			LOG_ERROR("receive incomplete", "JSON length mismatch");
			return CONN_ERROR;
		}
	}
	else
	{
		LOG_ERROR("response parse return error", "");
		return CONN_ERROR;
	}


	char* json_ptr = connection->buffer->pointer + header_size;

	if(DEBUG_RESTAPI)
	{
		LOG_DEBUG("response debug", "full response:[\n%s\n], json: [\n%s\n]", connection->buffer->pointer,json_ptr);
	}

	memcpy(connection->buffer->pointer, json_ptr, json_len);
	connection->buffer->pointer[json_len] = 0; 
	
	connection->buffer->len = json_len;

	if (ParseJson(connection) != PARSE_OK)
	{
		return PARSE_ERROR;
	}

	return CONN_OK;
}

static int AddHeader(connection_t *connection, const char* path)
{
	if (connection == NULL || connection->address == NULL || path == NULL || connection->buffer == NULL || connection->buffer->pointer == NULL)
	{
		LOG_ERROR("input argument in add header", "");
		return INVALID_ARGUMENT;
	}

	int json_len = connection->buffer->len;
	char* end = connection->buffer->pointer + json_len;


	//with this part i alloc necessery memory for header and get offset between header and json
	int request_len = snprintf(end, connection->buffer->size - json_len,
		"POST %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: %d\r\n"
		"Connection: keep-alive\r\n"
		"\r\n",
		path, connection->address, json_len);


	//failed to create header
	if (request_len < 0)
	{
		LOG_ERROR("failed to create header", "path=%s, host=%s, json=%s", path, connection->address, connection->buffer->pointer);
		return MEMORY_ERROR;
	}
	else if (connection->buffer->size - json_len < request_len)
	{
		if (ReallocArray(connection, (connection->buffer->size + request_len) * 2) != 0)
		{
			LOG_ERROR("header buffer overflow and realloc failed", "path=%s, host=%s, json=%s", path, connection->address, connection->buffer->pointer);
			return MEMORY_ERROR;
		}

		end = connection->buffer->pointer + connection->buffer->len;

		request_len = snprintf(end, connection->buffer->size - connection->buffer->len,
			"POST %s HTTP/1.1\r\n"
			"Host: %s\r\n"
			"Content-Type: application/json\r\n"
			"Content-Length: %d\r\n"
			"Connection: keep-alive\r\n"
			"\r\n",
			path, connection->address, connection->buffer->len);

		if (request_len < 0 || connection->buffer->size - connection->buffer->len < request_len)
		{
			LOG_ERROR("header overflow", "path=%s, host=%s, json=%s", path, connection->address, connection->buffer->pointer);
			return MEMORY_ERROR;
		}
	}

	//here i move json 
	memmove(connection->buffer->pointer + request_len, connection->buffer->pointer, json_len);

	request_len = snprintf(connection->buffer->pointer, connection->buffer->size,
		"POST %s HTTP/1.1\r\n"
		"Host: %s\r\n"
		"Content-Type: application/json\r\n"
		"Content-Length: %d\r\n"
		"Connection: keep-alive\r\n"
		"\r\n",
		path, connection->address, json_len);

	if (request_len < 0 || connection->buffer->size - json_len < request_len)
	{
		LOG_ERROR("header overflow on start of buffer", "path=%s, host=%s, json=%s", path, connection->address, connection->buffer->pointer);
		return MEMORY_ERROR;
	}
	connection->buffer->pointer[request_len] = '{';
	connection->buffer->len = request_len + json_len;

	return 0;
}

/* Send the prepared request buffer to the server. */
static int SendJson(connection_t* connection)
{

	if (connection == NULL || connection->buffer == NULL || connection->buffer->pointer == NULL)
	{
		LOG_ERROR("connection argument missing", "");
		return CONN_SETUP_ERROR;
	}
	//if socket is invalid create new and create connection
	if (connection->sock == INVALID_SOCKET)
	{
		if (CreateSocket(connection)) return CREATE_SOCKET_ERROR;
		if (ConnectWithTimeout(connection)) return CONN_TIMEOUT;
	}
	
	// send request to server in loop
	int total_sent = 0;
	int wsa_error = 0; //error variable for WSA error

	LOG_DEBUG("send json", "sending request: [\n%s\n]", connection->buffer->pointer);

	while (total_sent < connection->buffer->len)
	{	
		#ifdef _WIN32
		int sent = send(connection->sock, connection->buffer->pointer + total_sent, connection->buffer->len - total_sent, 0);
		#else
		ssize_t sent = send(connection->sock, connection->buffer->pointer + total_sent, connection->buffer->len - total_sent, 0);
		#endif
		if (sent == SOCKET_ERROR)
		{
			wsa_error = WSAGetLastError();
			/* WSAE* are mapped to POSIX errno values on non-Windows by restapi_compat.h */
			if (
				wsa_error == WSAECONNRESET   ||
				wsa_error == WSAENOTCONN     ||
				wsa_error == WSAECONNABORTED ||
				wsa_error == WSAETIMEDOUT    ||
				wsa_error == WSAENETRESET    ||
				wsa_error == WSAENETDOWN     ||
				wsa_error == WSAESHUTDOWN
			)
			{
				LOG_ERROR("disconnect", "wsa_error = % d", wsa_error);
				return CONN_NOT_CONNECTED;
			}
			else
			{
				LOG_ERROR("send failed", "wsa_error = % d", wsa_error);
				return CONN_ERROR;
			}
		}
		total_sent += sent;
	}

	return CONN_OK;
}

int JsonCommunication(connection_t*connection, const char* path, const char* json_text,...)
{
	if (api_init == 0)
	{
		LOG_ERROR("API is not initialized", "");
		return INIT_ERROR;
	}
	
	if (path == NULL || json_text == NULL)
	{
		LOG_ERROR("json or path argument missing", "");
		return CONN_SETUP_ERROR;
	}

	if (connection == NULL || connection->buffer == NULL || connection->buffer->pointer == NULL || connection->buffer->size <= 0)
	{
		LOG_ERROR("JsonCommunication failed", "invalid connection argument or buffer");
		return INVALID_ARGUMENT;
	}

	va_list args;
	int status = 0;

	for (int i = 0; i < ATTEMPTS; i++)
	{
		//create json for send
		memset(connection->buffer->pointer, 0 , connection->buffer->size);
		va_start(args, json_text);
		connection->buffer->len = vsnprintf(connection->buffer->pointer, connection->buffer->size, json_text, args);
		va_end(args);

		if (connection->buffer->len < 0)
		{
			LOG_ERROR("error while create request", "");
			return CREATE_BUFFER_ERROR;
		}

		else if (connection->buffer->len >= connection->buffer->size)
		{
			if (ReallocArray(connection, (connection->buffer->len)*2 )!= 0)
			{
				LOG_ERROR("failed to realloc memory for request", "");
				return MEMORY_ERROR;
			}

			va_start(args, json_text);
			connection->buffer->len = vsnprintf(connection->buffer->pointer, connection->buffer->size, json_text, args);
			va_end(args);

			if (connection->buffer->len < 0 || connection->buffer->len >= connection->buffer->size)
			{
				LOG_ERROR("error while create request", "");
				return CREATE_BUFFER_ERROR;
			}
		}

		if (AddHeader(connection, path) != 0)
		{
			LOG_ERROR("error while add header", "");
			return CREATE_BUFFER_ERROR;
		}

		if (i > 0)
		{
			LOG_DEBUG("Reconnect attempt", "");
			if (Reconnect(connection)) return CONN_SETUP_ERROR;
		}

		status = SendJson(connection);
		if (status == CONN_NOT_CONNECTED)
		{
			LOG_ERROR("send connection closed by server", "");
			continue; //try reconnect
		}
		else if (status != CONN_OK)
		{
			LOG_ERROR("send error while reading headers", "");
			return status;
		}

		status = RecvJson(connection);
		if (status == CONN_OK)
		{
			break; //break if succesfully recieved
		}
		else if (status == CONN_NOT_CONNECTED)
		{
			if (i == ATTEMPTS - 1) LOG_ERROR("recv connection closed by server", "");
			else LOG_DEBUG("recv connection closed by server", "");
			continue; //try reconnect
		}
		else if (status != CONN_OK)
		{
			LOG_ERROR("recv error while reading headers", "");
			return status;
		}
	}

	return 0;
}

void CleanupApi(void)
{
    #ifdef _WIN32
	if(api_init != 0)
	{
		WSACleanup();
	}
	#endif
}

/* --- Connection pool API --- */
void* CreateConnectionPool(char* host_addres, char* port, int thread_size)
{
	if (host_addres == NULL || port == NULL || thread_size <= 0)
	{
		return NULL;
	}

	init_pool_worker_t* init_t = (init_pool_worker_t*)calloc(1, sizeof(init_pool_worker_t));
	if (!init_t) return NULL;

	init_t->host_server = host_addres;
	init_t->port_server = port;
	init_t->init_func = PoolConnectionInitFunction;
	init_t->destroy_func = PoolConnectionDestroyFunc;
	init_t->worker_func = PoolConnectionWorkerFunc;

	// Create the pool with the specified number of threads
	pool_t* pool = CreatePool(thread_size, init_t);
	if (!pool)
	{
		free(init_t);
		return NULL;
	}
	

	return (void*)pool;
}

void DestroyConnectionPool(void* pool_v)
{
	pool_t* pool = (pool_t*)pool_v;
	if (pool)
	{
		init_pool_worker_t* init_data = pool->init_data;
		DestroyPool(pool);

		if (init_data)
		{
			free(init_data);
		}
	}
}

void* SendJsonToPool(void* pool_v, const char* path,const char* json, ...)
{
	if (pool_v == NULL || path == NULL || json == NULL)
	{
		LOG_ERROR("SendJsonToPool failed", "invalid input arguments");
		return NULL;
	}

	void* task = 0;
	va_list args;
	va_start(args, json);
	task = SendArgumentToPool(pool_v, path, json, args);
	va_end(args);
	return task;

}

void* SendArgumentToPool(void* pool_v, const char* path, const char* json, va_list args_in)
{
	pool_t*        pool      = (pool_t*)pool_v;
	task_t*        task      = NULL;
	dynamic_array* json_task = NULL;
	char*          path_task = NULL;
	va_list        argv;
	int            json_size, path_size, print_size;

	if (pool == NULL || path == NULL || json == NULL)
	{
		LOG_ERROR("SendJsonToPool failed", "invalid pool or json argument");
		return NULL;
	}

	task = CreateTask();
	if (task == NULL)
	{
		LOG_ERROR("SendJsonToPool failed", "failed to create task");
		return NULL;
	}

	/* calculate serialised json length */
	va_copy(argv, args_in);
	json_size = vsnprintf(NULL, 0, json, argv);
	va_end(argv);
	if (json_size < 0)
	{
		LOG_ERROR("SendJsonToPool failed", "error while calculating json size");
		goto free_all;
	}

	json_task = (dynamic_array*)calloc(1, sizeof(dynamic_array));
	if (json_task == NULL)
	{
		LOG_ERROR("SendJsonToPool failed", "failed to allocate memory for json task");
		goto free_all;
	}

	json_task->pointer = (char*)calloc(json_size + 1, sizeof(char));
	if (json_task->pointer == NULL)
	{
		LOG_ERROR("SendJsonToPool failed", "failed to allocate memory for json buffer");
		goto free_all;
	}

	va_copy(argv, args_in);
	print_size = vsnprintf(json_task->pointer, json_size + 1, json, argv);
	va_end(argv);
	if (print_size < 0 || print_size > json_size)
	{
		LOG_ERROR("SendJsonToPool failed", "error while serialising json");
		goto free_all;
	}

	path_size = snprintf(NULL, 0, "%s", path);
	if (path_size < 0)
	{
		LOG_ERROR("SendJsonToPool failed", "error while calculating path size");
		goto free_all;
	}

	path_task = (char*)calloc(path_size + 1, sizeof(char));
	if (path_task == NULL)
	{
		LOG_ERROR("SendJsonToPool failed", "failed to allocate memory for path");
		goto free_all;
	}

	print_size = snprintf(path_task, path_size + 1, "%s", path);
	if (print_size < 0 || print_size > path_size)
	{
		LOG_ERROR("SendJsonToPool failed", "error while copying path");
		goto free_all;
	}

	task->buffer       = json_task;
	task->path         = path_task;
	task->buffer->size = json_size + 1;

	if (AddTaskToPool(pool, task) != 0)
	{
		LOG_ERROR("SendJsonToPool failed", "failed to add task to pool");
		goto free_all;
	}

	return (void*)task;

free_all:
	if (task)      DestroyTask(task);
	if (json_task) { free(json_task->pointer); free(json_task); }
	if (path_task) free(path_task);
	return NULL;
}

void DeletePoolTask(void* task_v)
{
	task_t* task = (task_t*)task_v;
	if (task == NULL) return;
	DestroyTask(task);
	if (task->buffer) 
	{
		if (task->buffer->pointer) free(task->buffer->pointer);
		free(task->buffer);
	}
	if (task->path) free(task->path);
	return;
}

int WaitForConnectionDone(void* task_v)
{
	task_t* task = (task_t*)task_v;
	if (task == NULL)
	{
		LOG_ERROR("WaitForConnectionDone failed", "invalid task argument");
		return INVALID_ARGUMENT;
	}
	
	if (WaitForFinishTask(task) != 0)
	{
		LOG_ERROR("WaitForConnectionDone failed", "error while waiting for task to finish");
		return -1;
	}

	int status = atomic_int_get(&task->job_status);
	if (status != TASK_STATUS_FINISHED)
	{
		LOG_ERROR("WaitForConnectionDone failed", "task finished with error status: %d", status);
		return status;
	}

	return 0;
}

int GetConnectionResult(void* task_v)
{
	task_t* task = (task_t*)task_v;
	if (task == NULL)
	{
		LOG_ERROR("GetConnectionResult failed", "invalid task argument");
		return INVALID_ARGUMENT;
	}

	return atomic_int_get(&task->job_status);
}

char* GetConnectionParseJson(void* task_v)
{
	task_t* task = (task_t*)task_v;
	if (task == NULL || task->buffer == NULL || task->buffer->pointer == NULL)
	{
		LOG_ERROR("GetConnectionParseJson failed", "invalid task argument");
		return NULL;
	}

	return task->buffer->pointer;
}

int WaitForFinishConnectionPool(void* pool_v)
{
	pool_t* pool = (pool_t*)pool_v;
	if (pool == NULL)
	{
		LOG_ERROR("WaitForFinishConnectionPool failed", "invalid pool argument");
		return INVALID_ARGUMENT;
	}

	return WaitForFinishPool(pool);
}

static int PoolConnectionInitFunction(init_pool_worker_t* init_t, pool_worker_t* worker)
{
	if (init_t == NULL || worker == NULL)
	{
		LOG_ERROR("PoolConnectionInitFunction failed", "invalid init data");
		return -1;
	}

	connection_t* connection = CreateConnection(init_t->host_server, init_t->port_server);
	if (connection == NULL)
	{
		LOG_ERROR("PoolConnectionInitFunction failed", "failed to create worker connection");
		return -2;
	}

	worker->connection = connection;

	return 0;
}

static void PoolConnectionDestroyFunc(pool_worker_t* worker)
{
	if (worker == NULL || worker->connection == NULL)
	{
		LOG_ERROR("PoolConnectionDestroyFunc failed", "invalid worker argument");
		return;
	}

	DestroyConnection((connection_t*)worker->connection);
	return;
}

static int PoolConnectionWorkerFunc(pool_worker_t* worker)
{
	if (worker == NULL || worker->task == NULL || worker->connection == NULL || worker->task->buffer == NULL || worker->task->path == NULL || worker->task->buffer->pointer == NULL)
	{
		LOG_ERROR("PoolConnectionWorker failed", "invalid init data");
		return -1;
	}

	connection_t* connection = worker->connection;
	dynamic_array* buffer = worker->task->buffer;
	int json_size = worker->task->buffer->size;
	char* path = worker->task->path;

	if (JsonCommunication(connection, path, buffer->pointer) != CONN_OK)
	{
		LOG_ERROR("PoolConnectionWorker failed", "error during JsonCommunication");
		return -2;
	}

	if (connection->buffer->len > json_size)
	{
		char* temp_p = (char*)realloc(buffer->pointer, connection->buffer->len + 1);
		if (temp_p == NULL)
		{
			LOG_ERROR("PoolConnectionWorker failed", "failed to realloc memory for json result");
			return -3;
		}
		buffer->pointer = temp_p;
		buffer->size = connection->buffer->len + 1;
	}

	memcpy(buffer->pointer, connection->buffer->pointer, connection->buffer->len);
	buffer->pointer[connection->buffer->len] = '\0';
	worker->task->buffer->len = connection->buffer->len;

	return 0;
}

//static int 
//

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_WIN32)
#if defined(SCARLETTX_BUILDING_XCURL)
#define SCARLETTX_XCURL_API __declspec(dllexport)
#else
#define SCARLETTX_XCURL_API __declspec(dllimport)
#endif
#else
#define SCARLETTX_XCURL_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void CURL;
typedef int32_t CURLcode;
typedef int32_t CURLoption;
typedef int32_t CURLINFO;

typedef struct curl_slist {
    char* data;
    struct curl_slist* next;
} curl_slist;

typedef size_t (*curl_write_callback)(char* ptr, size_t size, size_t nmemb, void* userdata);

enum {
    CURLE_OK = 0,
    CURLE_UNSUPPORTED_PROTOCOL = 1,
    CURLE_URL_MALFORMAT = 3,
    CURLE_COULDNT_CONNECT = 7,
    CURLE_WRITE_ERROR = 23,
    CURLE_OPERATION_TIMEDOUT = 28,
    CURLE_BAD_FUNCTION_ARGUMENT = 43
};

// Standard libcurl option codes
enum {
    CURLOPT_WRITEDATA = 10001,
    CURLOPT_URL = 10002,
    CURLOPT_HTTPHEADER = 10023,
    CURLOPT_POSTFIELDS = 10015,
    CURLOPT_USERAGENT = 10018,
    CURLOPT_CUSTOMREQUEST = 10036,
    CURLOPT_WRITEFUNCTION = 20011,
    CURLOPT_TIMEOUT = 13,
    CURLOPT_POST = 47,
    CURLOPT_FOLLOWLOCATION = 52,
    CURLOPT_POSTFIELDSIZE = 60,
    CURLOPT_SSL_VERIFYPEER = 64
};

// Standard libcurl info codes
enum {
    CURLINFO_RESPONSE_CODE = 0x200002,
    CURLINFO_SIZE_DOWNLOAD_T = 0x600008,
    CURLINFO_CONTENT_TYPE = 0x100012
};

SCARLETTX_XCURL_API CURLcode curl_global_init(int64_t flags);
SCARLETTX_XCURL_API void curl_global_cleanup(void);
SCARLETTX_XCURL_API CURL* curl_easy_init(void);
SCARLETTX_XCURL_API void curl_easy_cleanup(CURL* curl);
SCARLETTX_XCURL_API void curl_easy_reset(CURL* curl);
SCARLETTX_XCURL_API CURLcode curl_easy_setopt(CURL* curl, CURLoption option, ...);
SCARLETTX_XCURL_API CURLcode curl_easy_perform(CURL* curl);
SCARLETTX_XCURL_API CURLcode curl_easy_getinfo(CURL* curl, CURLINFO info, ...);
SCARLETTX_XCURL_API const char* curl_easy_strerror(CURLcode code);
SCARLETTX_XCURL_API curl_slist* curl_slist_append(curl_slist* list, const char* string);
SCARLETTX_XCURL_API void curl_slist_free_all(curl_slist* list);
SCARLETTX_XCURL_API const char* curl_version(void);

// Multi interface (standard libcurl signatures and values).
typedef void CURLM;
typedef int32_t CURLMcode;

enum {
    CURLM_OK = 0,
    CURLM_BAD_HANDLE = 1,
    CURLM_BAD_EASY_HANDLE = 2,
    CURLM_ADDED_ALREADY = 7
};

enum { CURLMSG_DONE = 1 };

typedef struct CURLMsg {
    int32_t msg;
    CURL* easy_handle;
    union {
        void* whatever;
        CURLcode result;
    } data;
} CURLMsg;

struct curl_waitfd;

SCARLETTX_XCURL_API CURLM* curl_multi_init(void);
SCARLETTX_XCURL_API CURLMcode curl_multi_add_handle(CURLM* multi, CURL* curl);
SCARLETTX_XCURL_API CURLMcode curl_multi_remove_handle(CURLM* multi, CURL* curl);
SCARLETTX_XCURL_API CURLMcode curl_multi_perform(CURLM* multi, int* running_handles);
SCARLETTX_XCURL_API CURLMsg* curl_multi_info_read(CURLM* multi, int* msgs_in_queue);
SCARLETTX_XCURL_API CURLMcode curl_multi_poll(
    CURLM* multi,
    struct curl_waitfd* extra_fds,
    unsigned int extra_nfds,
    int timeout_ms,
    int* numfds);
SCARLETTX_XCURL_API CURLMcode curl_multi_cleanup(CURLM* multi);

// Deterministic offline mock registration for ScarlettX tests & offline play
SCARLETTX_XCURL_API CURLcode xcurl_register_mock_response(
    const char* url_prefix,
    int64_t response_code,
    const char* content_type,
    const void* body,
    size_t body_size);
SCARLETTX_XCURL_API void xcurl_clear_mocks(void);

#ifdef __cplusplus
}
#endif

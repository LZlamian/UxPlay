/*
 * DNS-SD for Windows without Bonjour (obs-airplay): see dnssd_win32.h.
 *
 * Registration goes through DnsServiceRegister, which the DNS Client
 * service answers over mDNS together with the computer's own host name
 * (<hostname>.local) and addresses. It completes asynchronously; a
 * registration that later fails (a name conflict, the service stopped) is
 * only seen in the completion status, which is kept with the service.
 * The API is looked up at run time so the module still loads on Windows
 * releases without it.
 */
#ifdef _WIN32

#include "dnssd_win32.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <winsock2.h>
#include <windows.h>
#include <windns.h>

#define TXT_MAX_ITEMS 64
#define DEREGISTER_WAIT_MS 2000

typedef PDNS_SERVICE_INSTANCE (WINAPI *construct_instance_t)(PCWSTR, PCWSTR, PIP4_ADDRESS,
                                                             PIP6_ADDRESS, WORD, WORD, WORD,
                                                             DWORD, PCWSTR *, PCWSTR *);
typedef DWORD (WINAPI *register_t)(PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL);
typedef VOID (WINAPI *free_instance_t)(PDNS_SERVICE_INSTANCE);

static construct_instance_t construct_instance;
static register_t service_register;
static register_t service_deregister;
static free_instance_t free_instance;

/* dns_sd.h leaves the service reference opaque; this is ours. */
struct _DNSServiceRef_t {
    DNS_SERVICE_REGISTER_REQUEST request;
    PDNS_SERVICE_INSTANCE instance;
    HANDLE completed;            /* set by every completion callback */
    volatile LONG status;        /* last completion status */
};

/* TXTRecordRef is 16 opaque bytes: they hold a pointer to this. */
typedef struct {
    uint8_t *bytes;
    uint16_t length;
} txt_t;

static txt_t **
txt_slot(TXTRecordRef *record) {
    return (txt_t **) (void *) record;
}

static txt_t *
txt_of(const TXTRecordRef *record) {
    return *(txt_t * const *) (const void *) record;
}

static void DNSSD_API
txt_create(TXTRecordRef *record, uint16_t buffer_length, void *buffer) {
    (void) buffer_length;
    (void) buffer;
    memset(record, 0, sizeof(*record));
    *txt_slot(record) = calloc(1, sizeof(txt_t));
}

static DNSServiceErrorType DNSSD_API
txt_set_value(TXTRecordRef *record, const char *key, uint8_t value_size, const void *value) {
    txt_t *txt = txt_of(record);
    if (!txt || !key) return kDNSServiceErr_BadParam;
    const size_t key_length = strlen(key);
    const size_t item = key_length + (value ? 1U + value_size : 0U);
    if (!key_length || item > 255 || (size_t) txt->length + 1U + item > UINT16_MAX) {
        return kDNSServiceErr_BadParam;
    }
    uint8_t *bytes = realloc(txt->bytes, (size_t) txt->length + 1U + item);
    if (!bytes) return kDNSServiceErr_NoMemory;
    txt->bytes = bytes;
    bytes += txt->length;
    *bytes++ = (uint8_t) item;
    memcpy(bytes, key, key_length);
    if (value) {
        bytes[key_length] = '=';
        memcpy(bytes + key_length + 1, value, value_size);
    }
    txt->length = (uint16_t) (txt->length + 1U + item);
    return kDNSServiceErr_NoError;
}

static uint16_t DNSSD_API
txt_get_length(const TXTRecordRef *record) {
    const txt_t *txt = txt_of(record);
    return txt ? txt->length : 0;
}

static const void * DNSSD_API
txt_get_bytes(const TXTRecordRef *record) {
    const txt_t *txt = txt_of(record);
    return txt ? txt->bytes : NULL;
}

static void DNSSD_API
txt_deallocate(TXTRecordRef *record) {
    txt_t *txt = txt_of(record);
    if (txt) free(txt->bytes);
    free(txt);
    *txt_slot(record) = NULL;
}

/* UTF-8 to a new UTF-16 string (caller frees), or NULL. */
static wchar_t *
widen(const char *text, int length) {
    if (length < 0) length = (int) strlen(text);
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, length, NULL, 0);
    if (count <= 0 && length > 0) return NULL;
    wchar_t *wide = calloc((size_t) count + 1U, sizeof(wchar_t));
    if (wide && count > 0) MultiByteToWideChar(CP_UTF8, 0, text, length, wide, count);
    return wide;
}

static void WINAPI
register_complete(DWORD status, PVOID context, PDNS_SERVICE_INSTANCE instance) {
    DNSServiceRef service = context;
    if (instance) free_instance(instance);  /* the callback owns this copy */
    InterlockedExchange(&service->status, (LONG) status);
    SetEvent(service->completed);
}

static void
service_free(DNSServiceRef service) {
    if (!service) return;
    if (service->instance) free_instance(service->instance);
    if (service->completed) CloseHandle(service->completed);
    free(service);
}

static DNSServiceErrorType DNSSD_API
native_register(DNSServiceRef *reference, DNSServiceFlags flags, uint32_t interface_index,
                const char *name, const char *type, const char *domain, const char *host,
                uint16_t port, uint16_t txt_length, const void *txt_record,
                DNSServiceRegisterReply callback, void *context) {
    (void) flags;
    (void) host;
    (void) callback;
    (void) context;
    *reference = NULL;
    if (!name || !type) return kDNSServiceErr_BadParam;

    /* "<name>.<type>.<domain>" and "<computer>.local" */
    char instance_name[512];
    const int written = snprintf(instance_name, sizeof(instance_name), "%s.%s.%s", name, type,
                                 domain && *domain ? domain : "local");
    if (written <= 0 || (size_t) written >= sizeof(instance_name)) return kDNSServiceErr_BadParam;
    wchar_t computer[256];
    DWORD computer_length = (DWORD) (sizeof(computer) / sizeof(computer[0]));
    if (!GetComputerNameExW(ComputerNameDnsHostname, computer, &computer_length)) {
        return kDNSServiceErr_Unknown;
    }
    wchar_t host_name[300];
    if (_snwprintf(host_name, sizeof(host_name) / sizeof(host_name[0]), L"%ls.local",
                   computer) < 0) {
        return kDNSServiceErr_BadParam;
    }

    /* TXT: length-prefixed "key=value" items to key and value arrays */
    PCWSTR keys[TXT_MAX_ITEMS] = {0};
    PCWSTR values[TXT_MAX_ITEMS] = {0};
    DWORD count = 0;
    DNSServiceErrorType error = kDNSServiceErr_NoError;
    const uint8_t *txt = txt_record;
    for (size_t offset = 0; txt && offset < txt_length && !error;) {
        const size_t item_length = txt[offset];
        if (offset + 1U + item_length > txt_length || count == TXT_MAX_ITEMS) {
            error = kDNSServiceErr_BadParam;
            break;
        }
        const char *item = (const char *) txt + offset + 1;
        const char *equals = memchr(item, '=', item_length);
        const int key_length = (int) (equals ? (size_t) (equals - item) : item_length);
        const char *value = equals ? equals + 1 : "";
        const int value_length = equals ? (int) (item_length - (size_t) key_length - 1U) : 0;
        keys[count] = widen(item, key_length);
        values[count] = widen(value, value_length);
        if (!keys[count] || !values[count]) error = kDNSServiceErr_BadParam;
        ++count;
        offset += 1U + item_length;
    }

    DNSServiceRef service = NULL;
    wchar_t *wide_name = error ? NULL : widen(instance_name, -1);
    if (!error && !wide_name) error = kDNSServiceErr_BadParam;
    if (!error) {
        service = calloc(1, sizeof(*service));
        if (service) service->completed = CreateEventW(NULL, FALSE, FALSE, NULL);
        if (!service || !service->completed) error = kDNSServiceErr_NoMemory;
    }
    if (!error) {
        /* the port arrives in network byte order */
        service->instance = construct_instance(wide_name, host_name, NULL, NULL, ntohs(port),
                                               0, 0, count, keys, values);
        if (!service->instance) error = kDNSServiceErr_NoMemory;
    }
    for (DWORD i = 0; i < count; ++i) {
        free((void *) keys[i]);
        free((void *) values[i]);
    }
    free(wide_name);

    if (!error) {
        service->status = DNS_REQUEST_PENDING;
        service->request.Version = DNS_QUERY_REQUEST_VERSION1;
        service->request.InterfaceIndex = interface_index;
        service->request.pServiceInstance = service->instance;
        service->request.pRegisterCompletionCallback = register_complete;
        service->request.pQueryContext = service;
        service->request.unicastEnabled = FALSE;
        const DWORD result = service_register(&service->request, NULL);
        if (result != DNS_REQUEST_PENDING) {
            error = result == ERROR_SERVICE_NOT_ACTIVE ? kDNSServiceErr_ServiceNotRunning
                                                       : kDNSServiceErr_Unknown;
        }
    }
    if (error) {
        service_free(service);
        return error;
    }
    *reference = service;
    return kDNSServiceErr_NoError;
}

static void DNSSD_API
native_deallocate(DNSServiceRef service) {
    if (!service) return;
    ResetEvent(service->completed);
    const DWORD result = service_deregister(&service->request, NULL);
    /* The completion callback still refers to the service: free it only
       once deregistration has completed, else leave it (a few hundred
       bytes) rather than risk a late callback into freed memory. */
    if (result == DNS_REQUEST_PENDING &&
        WaitForSingleObject(service->completed, DEREGISTER_WAIT_MS) != WAIT_OBJECT_0) {
        return;
    }
    service_free(service);
}

bool
dnssd_win32_native(dnssd_win32_functions_t *functions) {
    static HMODULE dnsapi;
    if (!dnsapi) dnsapi = LoadLibraryExW(L"dnsapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!dnsapi) return false;
    construct_instance = (construct_instance_t) (void *)
        GetProcAddress(dnsapi, "DnsServiceConstructInstance");
    service_register = (register_t) (void *) GetProcAddress(dnsapi, "DnsServiceRegister");
    service_deregister = (register_t) (void *) GetProcAddress(dnsapi, "DnsServiceDeRegister");
    free_instance = (free_instance_t) (void *) GetProcAddress(dnsapi, "DnsServiceFreeInstance");
    if (!construct_instance || !service_register || !service_deregister || !free_instance) {
        return false;
    }
    functions->DNSServiceRegister = native_register;
    functions->DNSServiceRefDeallocate = native_deallocate;
    functions->TXTRecordCreate = txt_create;
    functions->TXTRecordSetValue = txt_set_value;
    functions->TXTRecordGetLength = txt_get_length;
    functions->TXTRecordGetBytesPtr = txt_get_bytes;
    functions->TXTRecordDeallocate = txt_deallocate;
    return true;
}

#endif /* _WIN32 */

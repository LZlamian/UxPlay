/*
 * Windows without Bonjour: the DNS-SD calls dnssd.c makes, served by the
 * Windows DNS Client service's own mDNS responder (DnsServiceRegister,
 * Windows 10 1809 and later) and an in-process TXT record builder.
 * (obs-airplay)
 */
#ifndef DNSSD_WIN32_H
#define DNSSD_WIN32_H

#ifdef _WIN32
#include <stdbool.h>
#include <stdint.h>
#include <dns_sd.h>

typedef struct {
    DNSServiceErrorType (DNSSD_API *DNSServiceRegister)(DNSServiceRef *, DNSServiceFlags, uint32_t,
                                                        const char *, const char *, const char *,
                                                        const char *, uint16_t, uint16_t,
                                                        const void *, DNSServiceRegisterReply,
                                                        void *);
    void (DNSSD_API *DNSServiceRefDeallocate)(DNSServiceRef);
    void (DNSSD_API *TXTRecordCreate)(TXTRecordRef *, uint16_t, void *);
    DNSServiceErrorType (DNSSD_API *TXTRecordSetValue)(TXTRecordRef *, const char *, uint8_t,
                                                       const void *);
    uint16_t (DNSSD_API *TXTRecordGetLength)(const TXTRecordRef *);
    const void *(DNSSD_API *TXTRecordGetBytesPtr)(const TXTRecordRef *);
    void (DNSSD_API *TXTRecordDeallocate)(TXTRecordRef *);
} dnssd_win32_functions_t;

/* Fills `functions` when the Windows DNS service API exists (dnsapi.dll on
   Windows 10 1809+); false otherwise. */
bool dnssd_win32_native(dnssd_win32_functions_t *functions);
#endif

#endif

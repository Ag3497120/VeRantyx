// iphlpapi: the interface table (GetIfTable2, GetIfEntry2, FreeMibTable) agrees with itself and with
// GetAdaptersAddresses; GetInterfaceInfo lists IPv4 adapters by their \DEVICE\TCPIP_{GUID} names; DHCP leases are not
// released or renewed; change notifications are pending requests that CancelIPChangeNotify ends as aborted I/O.
// Only this Mac's own interfaces are looked at; nothing is changed.
#define _WIN32_WINNT 0x0A00                                      // (Windows 10: the Vista and Windows 8 declarations)
#include <winsock2.h>
#include <ws2tcpip.h>                                             // (before iphlpapi.h: its netioapi part needs ws2ipdef.h)
#include <iphlpapi.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int failures;
#define CHECK(c) do { if (!(c)) { printf("check failed (line %d): %s\n", __LINE__, #c); failures++; } } while (0)

// A notification request that no change completes, cancelled: pending, then aborted, its event signalled.
static void cancelled_request(DWORD (WINAPI *notify)(PHANDLE, LPOVERLAPPED), const char *name) {
    for (int attempt = 0; attempt < 3; attempt++) {
        OVERLAPPED ov;
        memset(&ov, 0, sizeof ov);
        ov.hEvent = CreateEventW(NULL, TRUE, TRUE, NULL);              // (signalled: the request resets it)
        HANDLE file = NULL;
        DWORD r = notify(&file, &ov);
        CHECK(r == ERROR_IO_PENDING);
        CHECK(file != NULL);
        DWORD bytes = 0;
        BOOL done = GetOverlappedResult(file, &ov, &bytes, FALSE);
        if (done) { printf("%s: a real change completed the request; again\n", name); CloseHandle(ov.hEvent); continue; }
        CHECK(GetLastError() == ERROR_IO_INCOMPLETE);
        CHECK(WaitForSingleObject(ov.hEvent, 0) == WAIT_TIMEOUT);
        HANDLE again = NULL;
        CHECK(notify(&again, &ov) == ERROR_INVALID_PARAMETER);       // the same request twice
        CHECK(CancelIPChangeNotify(&ov));
        CHECK(WaitForSingleObject(ov.hEvent, 2000) == WAIT_OBJECT_0);
        CHECK(!GetOverlappedResult(file, &ov, &bytes, FALSE) && GetLastError() == ERROR_OPERATION_ABORTED);
        CHECK(!CancelIPChangeNotify(&ov));                            // nothing pending any more
        CloseHandle(ov.hEvent);
        return;
    }
    printf("%s: the network kept changing; not checked\n", name);
}

int main(void) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // ---- the interface table
    PMIB_IF_TABLE2 table = NULL;
    CHECK(GetIfTable2(NULL) == ERROR_INVALID_PARAMETER);
    CHECK(GetIfTable2(&table) == NO_ERROR && table && table->NumEntries > 0);
    int loopbacks = 0;
    ULONG loopback_index = 0;
    for (ULONG i = 0; table && i < table->NumEntries; i++) {
        MIB_IF_ROW2 *row = &table->Table[i];
        CHECK(row->InterfaceIndex != 0);
        CHECK((row->InterfaceLuid.Value >> 48) == row->Type);           // LUID: IfType, then the index
        CHECK(((row->InterfaceLuid.Value >> 24) & 0xffffff) == row->InterfaceIndex);
        CHECK(row->Alias[0] != 0 && row->Description[0] != 0);
        CHECK(row->AdminStatus == NET_IF_ADMIN_STATUS_UP || row->AdminStatus == NET_IF_ADMIN_STATUS_DOWN);
        CHECK(row->OperStatus == IfOperStatusUp || row->OperStatus == IfOperStatusDown);
        CHECK(row->MediaConnectState == MediaConnectStateConnected || row->MediaConnectState == MediaConnectStateDisconnected);
        CHECK(row->PhysicalAddressLength <= IF_MAX_PHYS_ADDRESS_LENGTH);
        CHECK(row->InterfaceGuid.Data1 == 0x7a0b4c2d && row->InterfaceGuid.Data2 == 0x3e1f && row->InterfaceGuid.Data3 == 0x4a5b);
        if (row->Type == IF_TYPE_SOFTWARE_LOOPBACK) {
            loopbacks++;
            loopback_index = row->InterfaceIndex;
            CHECK(row->AccessType == NET_IF_ACCESS_LOOPBACK);
            CHECK(row->MediaType == NdisMediumLoopback);
            CHECK(row->OperStatus == IfOperStatusUp && row->MediaConnectState == MediaConnectStateConnected);
        }
    }
    CHECK(loopbacks >= 1);

    // GetIfEntry2: by index, by LUID; unknown ones are not found
    MIB_IF_ROW2 row;
    memset(&row, 0, sizeof row);
    CHECK(GetIfEntry2(NULL) == ERROR_INVALID_PARAMETER);
    CHECK(GetIfEntry2(&row) == ERROR_INVALID_PARAMETER);               // neither LUID nor index
    row.InterfaceIndex = loopback_index;
    CHECK(GetIfEntry2(&row) == NO_ERROR && row.Type == IF_TYPE_SOFTWARE_LOOPBACK && row.InterfaceIndex == loopback_index);
    for (ULONG i = 0; table && i < table->NumEntries; i++)            // the kernel's counters, read again: never fewer
        if (table->Table[i].InterfaceIndex == loopback_index)
            CHECK(row.InOctets >= table->Table[i].InOctets && row.OutUcastPkts >= table->Table[i].OutUcastPkts);
    NET_LUID luid = row.InterfaceLuid;
    memset(&row, 0, sizeof row);
    row.InterfaceLuid = luid;
    CHECK(GetIfEntry2(&row) == NO_ERROR && row.InterfaceIndex == loopback_index);
    memset(&row, 0, sizeof row);
    row.InterfaceIndex = 0xfffff0;
    CHECK(GetIfEntry2(&row) == ERROR_FILE_NOT_FOUND);

    // the adapter GetAdaptersAddresses reports is the same interface here: index, LUID, hardware address
    ULONG size = 0;
    if (GetAdaptersAddresses(AF_INET, 0, NULL, NULL, &size) == ERROR_BUFFER_OVERFLOW) {
        IP_ADAPTER_ADDRESSES *adapters = malloc(size);
        CHECK(GetAdaptersAddresses(AF_INET, 0, NULL, adapters, &size) == NO_ERROR);
        int matched = 0;
        for (ULONG i = 0; table && i < table->NumEntries; i++) {
            MIB_IF_ROW2 *r = &table->Table[i];
            if (r->InterfaceIndex != adapters->IfIndex) continue;
            matched = 1;
            CHECK(r->InterfaceLuid.Value == adapters->Luid.Value);
            CHECK(r->PhysicalAddressLength == adapters->PhysicalAddressLength &&
                  !memcmp(r->PhysicalAddress, adapters->PhysicalAddress, r->PhysicalAddressLength));
        }
        CHECK(matched);
        free(adapters);
    } else printf("iphlp: no IPv4 adapter; GetAdaptersAddresses not compared\n");
    FreeMibTable(table);

    // ---- IPv4 adapters
    ULONG need = 0;
    CHECK(GetInterfaceInfo(NULL, NULL) == ERROR_INVALID_PARAMETER);
    CHECK(GetInterfaceInfo(NULL, &need) == ERROR_INSUFFICIENT_BUFFER && need >= sizeof(IP_INTERFACE_INFO));
    IP_INTERFACE_INFO *info = malloc(need);
    DWORD r = GetInterfaceInfo(info, &need);
    CHECK(r == NO_ERROR || r == ERROR_NO_DATA);
    if (r == NO_ERROR) {
        CHECK(info->NumAdapters > 0 && need == sizeof(LONG) + info->NumAdapters * sizeof(IP_ADAPTER_INDEX_MAP));
        for (LONG i = 0; i < info->NumAdapters; i++) {
            CHECK(!wcsncmp(info->Adapter[i].Name, L"\\DEVICE\\TCPIP_{7A0B4C2D-3E1F-4A5B-9C8D-", 39) && wcslen(info->Adapter[i].Name) == 52);
            CHECK(info->Adapter[i].Index != loopback_index);
            MIB_IF_ROW2 a;
            memset(&a, 0, sizeof a);
            a.InterfaceIndex = info->Adapter[i].Index;
            CHECK(GetIfEntry2(&a) == NO_ERROR && a.AdminStatus == NET_IF_ADMIN_STATUS_UP);
        }
        // the Mac's DHCP leases stay as they are
        CHECK(IpReleaseAddress(&info->Adapter[0]) == ERROR_NOT_SUPPORTED);
        CHECK(IpRenewAddress(&info->Adapter[0]) == ERROR_NOT_SUPPORTED);
    } else printf("iphlp: no IPv4 adapter up; GetInterfaceInfo answered ERROR_NO_DATA\n");
    IP_ADAPTER_INDEX_MAP made_up = { 7, L"\\DEVICE\\TCPIP_{00000000-0000-0000-0000-000000000000}" };
    CHECK(IpReleaseAddress(NULL) == ERROR_INVALID_PARAMETER);
    CHECK(IpReleaseAddress(&made_up) == ERROR_INVALID_PARAMETER);
    CHECK(IpRenewAddress(&made_up) == ERROR_INVALID_PARAMETER);
    free(info);

    // ---- change notifications
    HANDLE h = NULL;
    OVERLAPPED ov = { 0 };
    CHECK(NotifyAddrChange(&h, NULL) == ERROR_INVALID_PARAMETER);
    CHECK(NotifyAddrChange(NULL, &ov) == ERROR_INVALID_PARAMETER);
    CHECK(!CancelIPChangeNotify(&ov));
    cancelled_request(NotifyAddrChange, "NotifyAddrChange");
    cancelled_request(NotifyRouteChange, "NotifyRouteChange");

    if (failures) { printf("iphlp: %d checks failed\n", failures); return 1; }
    puts("iphlp: interface table, entries, IPv4 adapters, DHCP leases left alone, change notifications: ok");
    return 0;
}

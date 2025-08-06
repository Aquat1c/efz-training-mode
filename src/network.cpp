#define WIN32_LEAN_AND_MEAN
#include <fstream>
#include <iomanip>
#include <chrono>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <tlhelp32.h>
#include <thread>
#include <vector>
#include <chrono>
#include "../include/utils/network.h"
#include "../include/core/logger.h"
#include "../include/utils/utilities.h"


#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

std::atomic<bool> isOnlineMatch(false);

// Helper function to get the EFZ process ID
DWORD GetEFZProcessID() {
    DWORD processID = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W processEntry;  // Note the 'W' suffix for Unicode version
        processEntry.dwSize = sizeof(PROCESSENTRY32W);
        
        if (Process32FirstW(snapshot, &processEntry)) {  // Also using 'W' version
            do {
                std::wstring processName = processEntry.szExeFile;
                if (processName == L"efz.exe" || 
                    processName == L"EFZ.exe" || 
                    processName == L"EfzRevival.exe") {
                    processID = processEntry.th32ProcessID;
                    break;
                }
            } while (Process32NextW(snapshot, &processEntry));  // 'W' version
        }
        CloseHandle(snapshot);
    }
    
    return processID;
}

// Check if the process has any network connections - improved for local network detection
bool DetectOnlineMatch() {
    DWORD processID = GetEFZProcessID();
    if (processID == 0) {
        return false;
    }
    
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }
    
    // Get TCP table (connections)
    MIB_TCPTABLE_OWNER_PID* pTcpTable = NULL;
    DWORD dwSize = 0;
    DWORD dwRetVal = 0;
    
    // First call to get the size of the table
    dwRetVal = GetExtendedTcpTable(NULL, &dwSize, TRUE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    
    if (dwRetVal == ERROR_INSUFFICIENT_BUFFER) {
        pTcpTable = (MIB_TCPTABLE_OWNER_PID*)malloc(dwSize);
        if (pTcpTable == NULL) {
            WSACleanup();
            return false;
        }
    }
    
    // Second call to get the actual data
    dwRetVal = GetExtendedTcpTable(pTcpTable, &dwSize, TRUE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    
    if (dwRetVal == NO_ERROR) {
        for (DWORD i = 0; i < pTcpTable->dwNumEntries; i++) {
            if (pTcpTable->table[i].dwOwningPid == processID) {
                if (pTcpTable->table[i].dwState == MIB_TCP_STATE_ESTAB || 
                    pTcpTable->table[i].dwState == MIB_TCP_STATE_LISTEN) {
                    free(pTcpTable);
                    WSACleanup();
                    return true;
                }
            }
        }
    }
    
    if (pTcpTable) free(pTcpTable);
    WSACleanup();
    
    // Also check UDP connections
    MIB_UDPTABLE_OWNER_PID* pUdpTable = NULL;
    dwSize = 0;
    
    // First call to get the size of the table
    dwRetVal = GetExtendedUdpTable(NULL, &dwSize, TRUE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    
    if (dwRetVal == ERROR_INSUFFICIENT_BUFFER) {
        pUdpTable = (MIB_UDPTABLE_OWNER_PID*)malloc(dwSize);
        if (pUdpTable == NULL) {
            return false;
        }
    }
    
    // Second call to get the actual data
    dwRetVal = GetExtendedUdpTable(pUdpTable, &dwSize, TRUE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    
    if (dwRetVal == NO_ERROR) {
        for (DWORD i = 0; i < pUdpTable->dwNumEntries; i++) {
            if (pUdpTable->table[i].dwOwningPid == processID) {
                free(pUdpTable);
                return true;
            }
        }
    }
    
    if (pUdpTable) free(pUdpTable);
    return false;
}

// Add this diagnostic function to log connection details when detected
void LogConnectionDetails(DWORD processID) {
    // Initialize Winsock
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return;
    }
    
    LogOut("[NETWORK] Logging active connections for EFZ (PID: " + std::to_string(processID) + ")", true);
    
    // TCP connections
    MIB_TCPTABLE_OWNER_PID* pTcpTable = NULL;
    DWORD dwSize = 0;
    GetExtendedTcpTable(NULL, &dwSize, TRUE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0);
    
    if (dwSize > 0) {
        pTcpTable = (MIB_TCPTABLE_OWNER_PID*)malloc(dwSize);
        if (pTcpTable && GetExtendedTcpTable(pTcpTable, &dwSize, TRUE, AF_INET, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
            bool foundAny = false;
            
            for (DWORD i = 0; i < pTcpTable->dwNumEntries; i++) {
                if (pTcpTable->table[i].dwOwningPid == processID) {
                    foundAny = true;
                    
                    // Get connection details
                    u_short localPort = ntohs((u_short)pTcpTable->table[i].dwLocalPort);
                    u_short remotePort = ntohs((u_short)pTcpTable->table[i].dwRemotePort);
                    
                    // Convert IPs to strings
                    char localAddr[INET_ADDRSTRLEN] = {0};
                    char remoteAddr[INET_ADDRSTRLEN] = {0};
                    
                    struct in_addr localIn, remoteIn;
                    localIn.s_addr = pTcpTable->table[i].dwLocalAddr;
                    remoteIn.s_addr = pTcpTable->table[i].dwRemoteAddr;
                    
                    inet_ntop(AF_INET, &localIn, localAddr, INET_ADDRSTRLEN);
                    inet_ntop(AF_INET, &remoteIn, remoteAddr, INET_ADDRSTRLEN);
                    
                    std::string state;
                    switch (pTcpTable->table[i].dwState) {
                        case MIB_TCP_STATE_ESTAB: state = "ESTABLISHED"; break;
                        case MIB_TCP_STATE_LISTEN: state = "LISTENING"; break;
                        default: state = "OTHER"; break;
                    }
                    
                    LogOut("[NETWORK] TCP: " + std::string(localAddr) + ":" + std::to_string(localPort) + 
                           " → " + std::string(remoteAddr) + ":" + std::to_string(remotePort) + 
                           " [" + state + "]", true);
                }
            }
            
            if (!foundAny) {
                LogOut("[NETWORK] No active TCP connections for EFZ", true);
            }
        }
        if (pTcpTable) free(pTcpTable);
    }
    
    // UDP connections
    MIB_UDPTABLE_OWNER_PID* pUdpTable = NULL;
    dwSize = 0;
    GetExtendedUdpTable(NULL, &dwSize, TRUE, AF_INET, UDP_TABLE_OWNER_PID, 0);
    
    if (dwSize > 0) {
        pUdpTable = (MIB_UDPTABLE_OWNER_PID*)malloc(dwSize);
        if (pUdpTable && GetExtendedUdpTable(pUdpTable, &dwSize, TRUE, AF_INET, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
            bool foundAny = false;
            
            for (DWORD i = 0; i < pUdpTable->dwNumEntries; i++) {
                if (pUdpTable->table[i].dwOwningPid == processID) {
                    foundAny = true;
                    
                    // Get socket details
                    u_short localPort = ntohs((u_short)pUdpTable->table[i].dwLocalPort);
                    
                    char localAddr[INET_ADDRSTRLEN] = {0};
                    struct in_addr localIn;
                    localIn.s_addr = pUdpTable->table[i].dwLocalAddr;
                    inet_ntop(AF_INET, &localIn, localAddr, INET_ADDRSTRLEN);
                    
                    LogOut("[NETWORK] UDP: " + std::string(localAddr) + ":" + std::to_string(localPort), true);
                }
            }
            
            if (!foundAny) {
                LogOut("[NETWORK] No active UDP connections for EFZ", true);
            }
        }
        if (pUdpTable) free(pUdpTable);
    }
    
    WSACleanup();
}

// Monitor for online status and manage console visibility
void MonitorOnlineStatus() {
    bool prevOnlineStatus = false;
    int checkCounter = 0;
    int consecutiveOnlineChecks = 0;
    bool loggedConnections = false;
    bool stopChecking = false;
    auto gameStartTime = std::chrono::steady_clock::now();
    
    while (true) {
        // Check if we should stop checking network status
        if (stopChecking) {
            // Just sleep and continue the loop without doing checks
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            continue;
        }

        // Check if game has been running for 10 seconds
        auto currentTime = std::chrono::steady_clock::now();
        auto elapsedSecs = std::chrono::duration_cast<std::chrono::seconds>(
            currentTime - gameStartTime).count();
        
        if (elapsedSecs > 10 && !isOnlineMatch.load()) {
            // Stop checking after 10 seconds if not online
            LogOut("[NETWORK] Stopped network monitoring (timeout)", false);
            stopChecking = true;
            continue;
        }
        
        // Check more frequently - every 2.5 seconds instead of 5
        if (++checkCounter >= 50) {
            checkCounter = 0;
            
            bool currentOnlineStatus = DetectOnlineMatch();
            
            if (currentOnlineStatus) {
                ++consecutiveOnlineChecks;
                
                // Log connection details on first detection
                if (!loggedConnections) {
                    DWORD processID = GetEFZProcessID();
                    if (processID != 0) {
                        LogConnectionDetails(processID);
                        loggedConnections = true;
                    }
                }
                
                // Reduce required checks from 3 to 2 for testing
                if (consecutiveOnlineChecks >= 2) {
                    LogOut("[NETWORK] Online match confirmed", true);
                    
                    // Reduced countdown from 5 to 3 seconds
                    for (int i = 3; i > 0; i--) {
                        LogOut("[NETWORK] Console will be hidden in " + std::to_string(i) + " seconds...", true);
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                    
                    // Hide console window
                    HWND consoleWnd = GetConsoleWindow();
                    if (consoleWnd != NULL) {
                        ShowWindow(consoleWnd, SW_HIDE);
                    }
                    
                    isOnlineMatch = true;
                    prevOnlineStatus = true;
                    
                    // Add this to stop checking after confirming online
                    stopChecking = true;
                }
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
// CDCTerm.cpp

// Simple terminal which auto-(re)connects 
// defaults to last USB COM port device connected
// override with command line COMnn

#include <Windows.h>
#include <stdio.h>
#include <conio.h>
#include "dbt.h"
#include "usbiodef.h"
#include "initguid.h"
#include "setupapi.h"

#pragma comment(lib, "setupAPI.lib")

#if 0

void handlerProc(DWORD control) { 
  printf("Control %d\n", control);
} 

HANDLE registerForDeviceEvents() { 
  
  // TODO: add rest of Microsoft service overcomplexity
  // CreateService();
  HANDLE hHandler = RegisterServiceCtrlHandler("USB Serial Hook", &handlerProc);  

  DEV_BROADCAST_DEVICEINTERFACE notificationFilter;
  ZeroMemory(&notificationFilter, sizeof(notificationFilter));
  notificationFilter.dbcc_size = sizeof(DEV_BROADCAST_DEVICEINTERFACE);
  notificationFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
  // GUID UsbGUID = { 0xA5DCBF10, 0x6530, 0x11D2, {0x90, 0x1F, 0x00, 0xC0, 0x4F, 0xB9, 0x51, 0xED} };
  // GUID UsbSerialGUID = { 0x25dbce51, 0x6c8f, 0x4a72, 0x8a,0x6d,0xb5,0x4c,0x2b,0x4f,0xc8,0x35 };
  notificationFilter.dbcc_classguid = GUID_CLASS_COMPORT;
  HANDLE hDeviceNotify = RegisterDeviceNotification(hHandler, &notificationFilter, DEVICE_NOTIFY_SERVICE_HANDLE);
  if (hDeviceNotify == NULL)  printf("Couldn't register for USB events!");

  return hDeviceNotify;
}
#endif


FILETIME latest = { 0 };

const char* lastActiveComPort() {
  static char comPortName[8] = "none";
  HDEVINFO hDevInfo = SetupDiGetClassDevs(&GUID_CLASS_COMPORT, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);

  for (int index = 0; ; index++) {
    SP_DEVINFO_DATA DeviceInfoData;
    DeviceInfoData.cbSize = sizeof(DeviceInfoData);
    if (!SetupDiEnumDeviceInfo(hDevInfo, index, &DeviceInfoData)) break;

    char hardwareID[256];
    SetupDiGetDeviceRegistryProperty(hDevInfo, &DeviceInfoData, SPDRP_HARDWAREID, NULL, (BYTE*)hardwareID, sizeof(hardwareID), NULL);  

    // truncate to make registry key for common USB CDC devices:
    char* truncate;
    if ((truncate = strstr(hardwareID, "&REV"))) *truncate = 0;
    if ((truncate = strstr(hardwareID, "\\COMPORT"))) *truncate = 0;  // FTDI 

    char devKeyName[256] = "System\\CurrentControlSet\\Enum\\";
    strcat_s(devKeyName, sizeof(devKeyName), hardwareID);
    HKEY devKey = 0;
    LSTATUS res = RegOpenKeyEx(HKEY_LOCAL_MACHINE, devKeyName, 0, KEY_READ, &devKey);
    if (devKey) {      
      DWORD idx = 0;
      while (1) {
        char serNum[64]; DWORD len = sizeof(serNum);
        FILETIME lastWritten = { 0 }; // 100 ns
        if (RegEnumKeyEx(devKey, idx++, serNum, &len, NULL, NULL, NULL, &lastWritten)) break;
        if (CompareFileTime(&lastWritten, &latest) > 0) { // latest device connected
          latest = lastWritten;
          if (strstr(devKeyName, "FTDIBUS")) strcat_s(serNum, sizeof(serNum), "\\0000"); // TODO: enumerate FTDI?
          strcat_s(serNum, sizeof(serNum), "\\Device Parameters");
          len = sizeof(comPortName);
          RegGetValue(devKey, serNum, "PortName", RRF_RT_REG_SZ, NULL, comPortName, &len);
          // SetupDiGetDeviceRegistryProperty(hDevInfo, &DeviceInfoData, SPDRP_FRIENDLYNAME, NULL, (BYTE*)devName, sizeof(devName), NULL);        
        }
      }
      RegCloseKey(devKey);
    } else if (strstr(hardwareID, "USB")) printf("%s not found", devKeyName); // low numbered non-removable ports ignored
  }

  SetupDiDestroyDeviceInfoList(hDevInfo);
  return comPortName;

  // see also HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\SERIALCOMM
  //   can check if disconnect removes from list
  //   can't tell which is last enumerated
}


HANDLE hCom;

HANDLE openSerial(const char* portName) {
  char portDev[16] = "\\\\.\\";
  strcat_s(portDev, sizeof(portDev), portName);

  hCom = CreateFile(portDev, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL); // better OVERLAPPED
  if (hCom == INVALID_HANDLE_VALUE) return NULL;

#if 0 // configure far end bridge COM port:
  DCB dcb;
  dcb.DCBlength = sizeof(DCB);
  dcb.BaudRate = 115200;
  // PL2303HX:  Divisor = 12 * 1000 * 1000 * 32 / baud --> 12M, 6M, 3M, 2457600, 1228800, 921600, ... baud
  // FTDI 3 MHz / (n + 0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875; where n is an integer between 2 and 16384)
  // Note: special cases n = 0 -> 3 MBaud; n = 1 -> 2 MBaud; Sub-integer divisors between 0 and 2 not allowed.
  dcb.ByteSize = 8;
  dcb.StopBits = 1;
  dcb.fBinary = TRUE; // no EOF check

#if 1
  dcb.fRtsControl = RTS_CONTROL_ENABLE;
  dcb.fDtrControl = DTR_CONTROL_ENABLE;

  dcb.fOutxDsrFlow = false;
  dcb.fOutxCtsFlow = false;
#else
  dcb.fRtsControl = RTS_CONTROL_HANDSHAKE;
  dcb.fDtrControl = DTR_CONTROL_HANDSHAKE;

  dcb.fOutxDsrFlow = true;
  dcb.fOutxCtsFlow = true;
#endif  

  if (!SetCommState(hCom, &dcb)) {
    printf("can't set baud ");
    return NULL;
  }
#endif

  if (!SetupComm(hCom, 16384, 16)) printf("can't SetupComm"); // Set size of I/O buffers (max 16384 on Win7)

  // USB bulk packets arrive at 1 kHz rate
  COMMTIMEOUTS timeouts = { 0 };  // in ms
  timeouts.ReadIntervalTimeout = 16; // overrides ReadTotalTimeoutConstant after first byte is received
  timeouts.ReadTotalTimeoutConstant = 16; // only in case of no device response
  timeouts.ReadTotalTimeoutMultiplier = 0;
  if (!SetCommTimeouts(hCom, &timeouts))  printf("can't SetCommTimeouts");

  return hCom;
}

int rxRdy(void) {
  COMSTAT cs;
  DWORD commErrors;
  if (!ClearCommError(hCom, &commErrors, &cs)) return -1;
  if (commErrors)
    printf("\n\rCommErr %X\n", commErrors); // 8 = framing (wrong baud rate); 2 = overrurn; 1 = overflow
  return cs.cbInQue;
}


int main(int argc, char** argv) {
#if 0
  HANDLE hDeviceNotify = registerForDeviceEvents();
  printf("watching...");
#endif

  const char* comPort;
  if (argc > 1)
    comPort = argv[1];
  else comPort = lastActiveComPort();

  while (1) {
    if (openSerial(comPort)) {
      printf("\nConnected to %s:\n", comPort);
      try {
        while (1) {
          if (_kbhit()) {
            char ch = _getch();
            if (!WriteFile(hCom, &ch, 1, NULL, NULL)) throw("close");
          }
          switch (rxRdy()) {
            case -1: throw("close");
            case 0: Sleep(16); break;
            default :
              char buf[64 + 1];
              DWORD bytesRead;
              if (!ReadFile(hCom, buf, sizeof(buf)-1, &bytesRead, NULL)) throw("close");
              buf[bytesRead] = 0;
              printf("%s", buf);
              break;
          }        
        }
      } catch (...) {
        CloseHandle(hCom);
      }
    } else Sleep(500);  // better on USB connect event
  }

  // UnregisterDeviceNotification(hDeviceNotify);
}

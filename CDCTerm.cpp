// CDCTerm.cpp

// Simple terminal which auto-(re)connects 
// defaults to last USB COM port device connected
// override with command line COMnn

// TODO:
//  Alt key to switch ports
//  backspace vs. Del 

#include <Windows.h>
#include <stdio.h>
#include <conio.h>
#include "dbt.h"
#include "usbiodef.h"
#include "initguid.h"
#include "setupapi.h"

#pragma comment(lib, "setupAPI.lib")

#if 0  // TODO: watch for USB events instead of polling registry

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


FILETIME prev = {MAXDWORD, MAXDWORD};

const char* lastActiveComPort() {
  FILETIME latest = { 0 };
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
        if (CompareFileTime(&lastWritten, &latest) > 0 && CompareFileTime(&lastWritten, &prev) < 0) { // latest device connected
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

  prev = latest;
  SetupDiDestroyDeviceInfoList(hDevInfo);
  return comPortName;

  // see also HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\SERIALCOMM
  //   can check if disconnect removes from list
  //   can't tell which is last enumerated
}


HANDLE hCom;

HANDLE openSerial(const char* portName, int baudRate = 921600) {
  char portDev[16] = "\\\\.\\";
  strcat_s(portDev, sizeof(portDev), portName);

  hCom = CreateFile(portDev, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL); // better OVERLAPPED
  if (hCom == INVALID_HANDLE_VALUE) return NULL;

 // configure far end bridge COM port - only for bridges - could check endpoint capabilites??
  DCB dcb = { 0 };
  dcb.DCBlength = sizeof(DCB);
  GetCommState(hCom, &dcb);

#if 1
  dcb.BaudRate = baudRate;
  // PL2303HX:  Divisor = 12 * 1000 * 1000 * 32 / baud --> 12M, 6M, 3M, 2457600, 1228800, 921600, ... baud
  // FTDI 3 MHz / (n + 0, 0.125, 0.25, 0.375, 0.5, 0.625, 0.75, 0.875; where n is an integer between 2 and 16384)
  // Note: special cases n = 0 -> 3 MBaud; n = 1 -> 2 MBaud; Sub-integer divisors between 0 and 2 not allowed.

  dcb.ByteSize = DATABITS_8;
  dcb.StopBits = 0; // STOPBITS_10;   // BUG in SetCommState or VCP??
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
#endif


  if (!SetCommState(hCom, &dcb)) {printf("Can't set baud\n"); }
  if (!SetupComm(hCom, 16384, 16)) printf("Can't SetupComm\n"); // Set size of I/O buffers (max 16384 on Win7)

  // USB bulk packets arrive at 1 kHz rate
  COMMTIMEOUTS timeouts = { 0 };  // in ms
  timeouts.ReadIntervalTimeout = 200;
  timeouts.ReadTotalTimeoutConstant = 16;
  timeouts.ReadTotalTimeoutMultiplier = 0;
  if (!SetCommTimeouts(hCom, &timeouts))  printf("Can't SetCommTimeouts\n");

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

bool EnableVTMode() { // Handle VT100 terminal sequences  
  HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
  if (hOut == INVALID_HANDLE_VALUE) return false;

  DWORD dwMode = 0;
  if (!GetConsoleMode(hOut, &dwMode)) return false;
  dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING; //  | ENABLE_QUICK_EDIT_MODE;
  return SetConsoleMode(hOut, dwMode);
}

#define KEY_F1         59
#define KEY_F2         60
#define KEY_F3         61
#define KEY_F4         62
#define KEY_F5         63
#define KEY_F6         64
#define KEY_F7         65
#define KEY_F8         66
#define KEY_F9         67

#define KEY_HOME       71
#define KEY_UP         72
#define KEY_PGUP       73
#define KEY_LEFT       75
#define KEY_CENTER     76
#define KEY_RIGHT      77
#define KEY_END        79
#define KEY_DOWN       80
#define KEY_PGDN       81
#define KEY_INSERT     82
#define KEY_DELETE     83

void escapeKeys() {
  char ch = _getch();
  switch (ch) { // VT100 Escape sequences 
    case KEY_UP:    ch = 'A'; break;
    case KEY_DOWN:  ch = 'B'; break;
    case KEY_RIGHT: ch = 'C'; break;
    case KEY_LEFT:  ch = 'D'; break;

    case KEY_HOME:  ch = 'H'; break;
    case KEY_END:   ch = 'F'; break;

    default :
      switch (ch) {
        case KEY_HOME:   ch = '1'; break;
        case KEY_INSERT: ch = '2'; break;
        case KEY_DELETE: ch = '3'; break;
        case KEY_END:    ch = '4'; break;
        case KEY_PGUP:   ch = '5'; break;
        case KEY_PGDN:   ch = '6'; break;
        default : return;
      }
      char csi4[] = "\x1B" "[n~";
      csi4[2] = ch;
      WriteFile(hCom, &csi4, 4, NULL, NULL);
      return;
  }
  char CSI[3] = "\x1B" "[";
  CSI[2] = ch;
  if (!WriteFile(hCom, &CSI, 3, NULL, NULL)) throw "close";
}


BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
  char ch = 3; // ^C
  WriteFile(hCom, &ch, 1, NULL, NULL);  
  return true;
}

int main(int argc, char** argv) {
#if 0
  HANDLE hDeviceNotify = registerForDeviceEvents();
  printf("watching...");
#endif

  SetConsoleCtrlHandler(CtrlHandler, TRUE);  // doesn't work when run under debugger
  EnableVTMode();

  const char* comPort;
  if (argc > 1)
    comPort = argv[1];
  else comPort = lastActiveComPort();

  int baudRate = 921600; // or 115200   -- doesn't matter unless bridged
  if (argc > 2)
    baudRate = atoi(argv[2]);

  while (!openSerial(comPort, baudRate))
    comPort = lastActiveComPort();

  while (1) {
    SetWindowText(GetConsoleWindow(), comPort);
    printf("\nConnected to %s:\n", comPort);
    try {
      while (1) {
        while (_kbhit()) {
          unsigned char ch = _getch();           
          if (ch == 0 || ch == 0xE0)
            escapeKeys(); // handle arrow keys: 0 or 0xE0 followed by key code
          else if (!WriteFile(hCom, &ch, 1, NULL, NULL)) throw "close";
        }
        switch (rxRdy()) {
          case -1: throw "close";
          case 0: Sleep(16); break;
          default :
            char buf[64 * 2 + 1]; // two USB buffers
            DWORD bytesRead;
            if (!ReadFile(hCom, buf, sizeof(buf)-1, &bytesRead, NULL)) throw "close";
            buf[bytesRead] = 0;      

            while (1) { // remove ^Os  at both ends of  top  lines
              char* p = strchr(buf, 15);
              if (!p) break;
              memmove(p, p + 1, bytesRead + buf - p);  // remove ^O
            }

            printf("%s", buf);
            break;
        }        
      }
    } catch (...) {
      if (hCom > 0) CloseHandle(hCom);
    }

    if (!openSerial(comPort, baudRate)) Sleep(500);  // better on USB reconnect event
  }

  // UnregisterDeviceNotification(hDeviceNotify);
}

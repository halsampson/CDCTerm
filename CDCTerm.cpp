// CDCTerm.cpp

// Simple terminal which auto-(re)connects
// defaults to last free USB COM port device connected
// override with command line [COMnn] [baud]
// or launch more instances to cycle through ports

// TODO:
//  backspace vs. Del 
//  Alt key to switch ports (or run another instance)
//  Esc seq to write line to CSV file

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

char friendlyName[128], commName[128];

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
    if (strstr(hardwareID, "VID_1CBE")) 
      strcat_s(hardwareID, sizeof(hardwareID), "&MI_00"); // Stellaris

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
          len = sizeof(friendlyName);
          if (RegGetValue(devKey, serNum, "FriendlyName", RRF_RT_REG_SZ, NULL, friendlyName, &len) != ERROR_SUCCESS)
            if (strstr(devKeyName, "FTDIBUS")) {
              strcat_s(serNum, sizeof(serNum), "\\0000");
              RegGetValue(devKey, serNum, "FriendlyName", RRF_RT_REG_SZ, NULL, friendlyName, &len);
            }

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

  // move COMxx to start of friendlyName
  char* comPort = strchr(friendlyName, '(');
  if (comPort) {
    strcpy_s(commName, sizeof(commName), comPort+1);
    *strchr(commName, ')') = ' ';
    strcat_s(commName, sizeof(commName), friendlyName);
    *(strchr(commName, '(') - 1) = 0;
  } else strcpy_s(commName, sizeof(commName), friendlyName);

  return comPortName;

  // see also HKEY_LOCAL_MACHINE\HARDWARE\DEVICEMAP\SERIALCOMM
  //   can check if disconnect removes from list
  //   can't tell which is last enumerated
}


HANDLE hCom = NULL;

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
 	// CP2104 24 MHz / N 
  // FTDI   3MHz, 2MHz, then 24 MHz / N for N >= 16
	// PL2303HX: 12 MHz * 32 / prescale / {1..255} > 115200
     // https://elixir.bootlin.com/linux/v3.10/source/drivers/usb/serial/pl2303.c#L364
  // CP2102: 24 MHz / N >= 8 from 32 entry programmable table   1 MHz max?
	// CH340:  12 MHz / {1, 2, 8, 16, 64, 128, 512, 1024} / {2..256 or 9..256 when prescale = 1}
    // https://github.com/nospam2000/ch341-baudrate-calculation

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
  static DWORD lastCommErrors;
  if (commErrors && commErrors != lastCommErrors) { // else annoying
    lastCommErrors = commErrors;
    printf("\n\rCommErr %X\n", commErrors); // 16 = CE_BREAK; 8 = framing (wrong baud rate); 2 = overrun; 1 = overflow  
  }
  return cs.cbInQue;
}

void setInputEcho(bool on = true) {
  HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
  DWORD mode = 0;
  GetConsoleMode(hStdin, &mode);
  if (on) mode |= ENABLE_ECHO_INPUT;
  else mode &= ~ENABLE_ECHO_INPUT;
//  mode &= ~ENABLE_LINE_INPUT;
  SetConsoleMode(hStdin, mode);
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

#define KEY_F12       134

enum { off, minLSB, minMSB, maxLSB, maxMSB, binLSB, binMSB } binMode;

void escapeKeys(unsigned char ch) {  // for TERM=vt100 
  switch (ch) { // VT100 Escape sequences 
    case KEY_UP:    ch = 'A'; break;
    case KEY_DOWN:  ch = 'B'; break;
    case KEY_RIGHT: ch = 'C'; break;
    case KEY_LEFT:  ch = 'D'; break;

    case KEY_HOME:  ch = 'H'; break;
    case KEY_END:   ch = 'F'; break;

    case KEY_F12:
      binMode = off;
      system("cls"); 
      return;  

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


unsigned char processKey() { 
  unsigned char ch = _getch();
  if (ch == 0 || ch == 0xE0) {
    escapeKeys(_getch()); // handle arrow keys: 0 or 0xE0 followed by key code
    return 0;
  } else if (!WriteFile(hCom, &ch, 1, NULL, NULL)) throw "close";
  return ch;
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
  char ch = 3; // ^C
  WriteFile(hCom, &ch, 1, NULL, NULL);  
  return true;
}

HDC plot;
int maxX, maxY;

// TODO: better two threads

void processComms() {
  switch (rxRdy()) {
    case -1: throw "close";
    case 0: // no incoming comms
      Sleep(50);
      break;
    default:
      unsigned char buf[64 * 2 + 1]; // two USB buffers
      DWORD bytesRead;
      if (!ReadFile(hCom, (char*)buf, sizeof(buf)-1, &bytesRead, NULL)) throw "close";
      buf[bytesRead] = 0;

      static int s, minADC, maxADC, x, binChs, binCh;
      for (DWORD p = 0; p < bytesRead; ++p) {
        switch (binMode) {
          case off : 
            if (0 && (buf[p] & 0xF0) == 0xB0) {    // too easy to enter ***  TODO: require another mode character
              binChs = buf[p] & 0xF;
              binMode = minLSB;
              binCh = 0;
              x = 0;
            }
            break;

          case minLSB:
            minADC = buf[p];
            binMode = minMSB;
            break;

          case minMSB:
            minADC |= buf[p] << 8;
            binMode = maxLSB;
            break;

          case maxLSB:
            maxADC = buf[p];
            binMode = maxMSB;
            break;

          case maxMSB:
            maxADC |= buf[p] << 8;
            binMode = binLSB;
            break;

          case binLSB:
            s = buf[p];
            binMode = binMSB;
            break;

          case binMSB:
            s |= buf[p] << 8;
            if (s == 0xFFFF) { // terminate plot
              binMode = off;
              printf("%s", buf + p);
              return;
            }
            binMode = binLSB;
            // TODO: color, gain per binCh
            s = (s - minADC) * maxY / (maxADC - minADC);
            SetPixel(plot, x, maxY - s, RGB(255, 128, 128)); 
            if (++binCh >= binChs) {
              binCh = 0;
              if (++x >= maxX) x = 0;
            }
            break;
        }
      }
      if (binMode != off) break;
      
      while (1) { // remove ^Os  at both ends of  top  lines
        unsigned char* p = (unsigned char*)strchr((char*)buf, 15);
        if (!p) break;
        memmove(p, p + 1, bytesRead + buf - p);  // remove ^O
      }

      printf("%s", buf);
      break;
  }
}


int main(int argc, char** argv) {
#if 0
  HANDLE hDeviceNotify = registerForDeviceEvents();
  printf("watching...");
#endif

  HWND consoleWnd = GetConsoleWindow();
  plot = GetDC(consoleWnd);
  RECT rect;

  SetConsoleCtrlHandler(CtrlHandler, TRUE);  // doesn't work when run under debugger
  EnableVTMode();

  // args in any order "bbbbb" or "COMnn"

  int baudRate = 921600; // 115200   -- doesn't matter unless bridged
  const char* comPort = NULL;

  for (int arg = 1; arg < argc; arg++) {
    int val = atoi(argv[arg]);
    if (val)
      baudRate = val;
    else comPort = argv[arg];
  }

  if (!comPort)
    comPort = lastActiveComPort();  // "COMnn"

  while (!openSerial(comPort, baudRate))
    comPort = lastActiveComPort();

  if (hCom <= (HANDLE)0) exit(-1);

  SetWindowText(GetConsoleWindow(), commName);
  printf("\nConnected to %s\n", commName);

  while (1) {
    try {
      while (1) {
        int pasteCount = 0;
        while (_kbhit()) {
          GetClientRect(consoleWnd, &rect);
          maxX = rect.right;
          maxY = rect.bottom;

          unsigned char ch = processKey();
          if (!ch) break; // Escape seq
          if (ch == ' ') {
            binMode = off;
            InvalidateRect(consoleWnd, NULL, true); // clear plot
          }
          if (ch <= '\r') {
            ++pasteCount;
            SetWindowText(GetConsoleWindow(), commName);  // typing: restore title after select
          }

          while (pasteCount >= 2 && _kbhit()) { 
            // pasting, not typing; process line at a time for speed -> Must CR at end of pasted text!!!
            if (pasteCount++ == 2) setInputEcho(false);
            char buf[64 * 2 + 1]; // two USB buffers
            fgets(buf, sizeof(buf)-1, stdin); // to buffer or newline  
            DWORD len = (DWORD)strlen(buf);
            if (!WriteFile(hCom, buf, len, NULL, NULL)) throw "close";
            processComms();
          }
        }
        setInputEcho(true);
        processComms();
      }
    } catch (...) {
      if (hCom > (HANDLE)0) {
        CloseHandle(hCom);
        hCom = 0;
      }
    }

    while (!openSerial(comPort, baudRate)) Sleep(1000);  // better on USB reconnect event
  }

  // UnregisterDeviceNotification(hDeviceNotify);
}

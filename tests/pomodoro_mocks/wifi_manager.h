// Host-test mock: app_pomodoro.cc only polls connection state for the shared
// status bar; the real singleton lives in components/esp-wifi-connect.
#pragma once
class WifiManager {
public:
    static WifiManager &GetInstance() { static WifiManager w; return w; }
    bool IsConnected() { return false; }
    int GetRssi() { return -100; }
};

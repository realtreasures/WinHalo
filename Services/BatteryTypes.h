#pragma once
namespace winisland
{
struct BatterySnapshot
{
    bool available = false;
    bool onBattery = false;
    bool charging = false;
    int percent = -1;
    bool low = false;
};
}

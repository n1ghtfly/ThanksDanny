// Phone link (v3.38): the badge as a Bluetooth LE media remote for a phone, plus iPhone notifications.
// Plain C++ in src/ (not an .ino) so Arduino's auto-prototypes never see the NimBLE types.
#pragma once
#include <stdint.h>

// Media keys (bits of the HID consumer-control report)
#define PHONE_KEY_NEXT   0
#define PHONE_KEY_PREV   1
#define PHONE_KEY_STOP   2
#define PHONE_KEY_PLAY   3      // play / pause
#define PHONE_KEY_MUTE   4
#define PHONE_KEY_VOLUP  5
#define PHONE_KEY_VOLDN  6

// Notification categories (Apple ANCS CategoryID)
#define PHONE_CAT_OTHER   0
#define PHONE_CAT_CALL    1
#define PHONE_CAT_MISSED  2
#define PHONE_CAT_VOICE   3
#define PHONE_CAT_SOCIAL  4
#define PHONE_CAT_SCHED   5
#define PHONE_CAT_EMAIL   6
#define PHONE_CAT_NEWS    7

void phoneBegin(const char *deviceName);   // start Bluetooth, advertise as "<name>" (call once)
bool phoneStarted();
bool phoneConnected();                     // a phone is connected
bool phoneSecure();                        // ... and the link is encrypted (paired)
bool phoneNotifyReady();                   // iPhone notification service subscribed
void phoneMediaKey(int key);               // press + release one media key
void phoneAdvertise(bool on);              // make the badge (in)visible for new connections
void phoneForget();                        // delete all pairings (phones must pair again)
// Next notification from the phone, if one arrived (ASCII, cut to the buffer sizes). True if one was taken.
bool phoneNextNotification(char *app, int appLen, char *title, int titleLen, char *msg, int msgLen, int *category);

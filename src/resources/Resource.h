#pragma once

// App icon & Logo
#define IDI_QUICKVIEW       101
#define IDB_APP_LOGO        102

// Hotkey
#define HOTKEY_ID_PREVIEW   1001

// Tray icon & Commands
#define WM_TRAYICON             (WM_USER + 100)
#define IDM_EXIT                2001
#define IDM_START_WITH_WINDOWS  2002
#define IDM_VIDEO_SAVE_IMAGE    2010
#define IDM_VIDEO_COPY_PATH     2011
#define IDM_VIDEO_FULLSCREEN    2012
#define IDM_VIDEO_COPY_FOLDER   2013
#define IDM_COPY_FILE_NAME      2014
#define IDM_COPY_TIMECODE       2015

// Timers
#define TIMER_FADE              3001
#define TIMER_WATCH_SELECTION   3003
#define TIMER_LOADING_SPINNER   3004

// App Messages
#define WM_APP_VIDEO_CLICK      (WM_APP + 201)
#define WM_APP_FILE_LOADED      (WM_APP + 202)
#define WM_APP_VIDEO_RCLICK     (WM_APP + 203)

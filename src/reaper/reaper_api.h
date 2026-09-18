#pragma once
// Every REAPER API function the extension uses is listed here once. With
// REAPERAPI_MINIMAL only these are declared, and reaper_api.cpp resolves them
// at load time. Add a REAPERAPI_WANT_ line before using a new function.

#define REAPERAPI_MINIMAL
#define REAPERAPI_WANT_ShowConsoleMsg
#define REAPERAPI_WANT_ShowMessageBox
#define REAPERAPI_WANT_GetMainHwnd
#define REAPERAPI_WANT_GetResourcePath
#define REAPERAPI_WANT_GetAppVersion
#define REAPERAPI_WANT_plugin_register
#define REAPERAPI_WANT_Undo_BeginBlock2
#define REAPERAPI_WANT_Undo_EndBlock2
#define REAPERAPI_WANT_UpdateArrange
#define REAPERAPI_WANT_CountSelectedMediaItems
#define REAPERAPI_WANT_GetSelectedMediaItem
#define REAPERAPI_WANT_GetActiveTake
#define REAPERAPI_WANT_GetTakeName
#define REAPERAPI_WANT_GetMediaItemTake_Source
#define REAPERAPI_WANT_GetMediaItemTake_Item
#define REAPERAPI_WANT_GetMediaItemTrack
#define REAPERAPI_WANT_GetMediaItemInfo_Value
#define REAPERAPI_WANT_SetMediaItemInfo_Value
#define REAPERAPI_WANT_GetMediaItemTakeInfo_Value
#define REAPERAPI_WANT_GetSetMediaItemInfo_String
#define REAPERAPI_WANT_SetMediaItemPosition
#define REAPERAPI_WANT_SetMediaItemLength
#define REAPERAPI_WANT_UpdateItemInProject
#define REAPERAPI_WANT_GetItemProjectContext
#define REAPERAPI_WANT_ValidatePtr2
#define REAPERAPI_WANT_GetMediaSourceType
#define REAPERAPI_WANT_GetMediaSourceLength
#define REAPERAPI_WANT_GetMediaSourceSampleRate
#define REAPERAPI_WANT_GetMediaSourceNumChannels
#define REAPERAPI_WANT_GetTakeNumStretchMarkers
#define REAPERAPI_WANT_GetTakeStretchMarker
#define REAPERAPI_WANT_SetTakeStretchMarker
#define REAPERAPI_WANT_DeleteTakeStretchMarkers
#define REAPERAPI_WANT_TimeMap2_beatsToTime
#define REAPERAPI_WANT_TimeMap_GetMeasureInfo
#define REAPERAPI_WANT_TimeMap_GetDividedBpmAtTime
#define REAPERAPI_WANT_format_timestr_pos

#include "reaper_plugin.h"
#include "reaper_plugin_functions.h"

// SWELL defines min/max as macros, which breaks <algorithm> and <limits>.
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

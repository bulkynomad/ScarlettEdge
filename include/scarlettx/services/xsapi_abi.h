#pragma once

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_XSAPI)
#    define SCARLETTX_XSAPI_API __declspec(dllexport)
#  else
#    define SCARLETTX_XSAPI_API __declspec(dllimport)
#  endif
#  define SCARLETTX_XSAPI_CALL __stdcall
#else
#  define SCARLETTX_XSAPI_API __attribute__((visibility("default")))
#  define SCARLETTX_XSAPI_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void* XblContextHandle;
typedef void* XblMultiplayerSessionHandle;
typedef void* XblAchievementsResultHandle;

typedef struct XblUserProfile {
    uint64_t xboxUserId;
    char gamertag[32];
    char modernGamertag[32];
    char modernGamertagSuffix[16];
    char uniqueModernGamertag[48];
    char gamerscore[16];
    char gameDisplayPictureResizeUri[256];
} XblUserProfile;

// Synthetic offline Xbox Live service initialization
SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblInitialize(const char* scid);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblCleanup(void);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblContextCreateHandle(
    uint64_t xuid, XblContextHandle* outContext);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblContextCloseHandle(
    XblContextHandle context);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblContextGetXboxUserId(
    XblContextHandle context, uint64_t* outXboxUserId);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblContextGetGamertag(
    XblContextHandle context, char* outGamertag, size_t bufferSize);

// Achievements
SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblAchievementsGetAchievementsForTitleIdAsync(
    XblContextHandle context, uint32_t titleId, void* asyncBlock);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblAchievementsGetAchievementsForTitleIdResult(
    void* asyncBlock, size_t bufferSize, void* buffer, XblAchievementsResultHandle* resultHandle, size_t* bufferUsed);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblAchievementsUpdateAchievementAsync(
    XblContextHandle context, const char* achievementId, uint32_t percentComplete, void* asyncBlock);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblAchievementsUpdateAchievementResult(
    void* asyncBlock);

// User Profile
SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblProfileGetUserProfileAsync(
    XblContextHandle context, uint64_t xboxUserId, void* asyncBlock);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblProfileGetUserProfileResult(
    void* asyncBlock, size_t bufferSize, void* buffer, XblUserProfile* userProfile, size_t* bufferUsed);

// Multiplayer session management
SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblMultiplayerCreateSession(
    XblContextHandle context, const char* sessionTemplateName, XblMultiplayerSessionHandle* outSession);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblMultiplayerLeaveSession(
    XblMultiplayerSessionHandle session);

// Rich presence
SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblPresenceSetPresenceAsync(
    XblContextHandle context, int32_t isUserActive, const char* richPresenceString, void* asyncBlock);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblPresenceSetPresenceResult(
    void* asyncBlock);

// Persistent disk-backed XblTitleStorage (GDK 2022-2026 Cloud/Local Title Storage)
enum XblTitleStorageType {
    XblTitleStorageType_TrustedPlatformStorage = 0,
    XblTitleStorageType_GlobalStorage = 2,
    XblTitleStorageType_Universal = 3,
};

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblTitleStorageUploadBlobAsync(
    XblContextHandle context,
    const char* blobPath,
    uint32_t storageType,
    const uint8_t* blobBuffer,
    size_t blobBufferSize,
    void* asyncBlock);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblTitleStorageUploadBlobResult(
    void* asyncBlock);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblTitleStorageDownloadBlobAsync(
    XblContextHandle context,
    const char* blobPath,
    uint32_t storageType,
    uint8_t* blobBuffer,
    size_t blobBufferSize,
    void* asyncBlock);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblTitleStorageDownloadBlobResult(
    void* asyncBlock,
    size_t* bytesDownloaded);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblTitleStorageDeleteBlobAsync(
    XblContextHandle context,
    const char* blobPath,
    uint32_t storageType,
    void* asyncBlock);

SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblTitleStorageDeleteBlobResult(
    void* asyncBlock);

// WinRT RoActivateInstance / RoGetActivationFactory Xbox Game Core Broker
SCARLETTX_XSAPI_API int32_t SCARLETTX_XSAPI_CALL XblWinRtActivateClassByName(
    const wchar_t* runtimeClassName,
    const void* riid,
    int32_t isFactory,
    void** outInspectable);

#ifdef __cplusplus
}
#endif

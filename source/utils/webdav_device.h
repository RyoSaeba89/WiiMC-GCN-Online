#ifndef WIIMC_WEBDAV_DEVICE_H
#define WIIMC_WEBDAV_DEVICE_H
#ifdef __cplusplus
extern "C" {
#endif
void WebDAVInit(const char *app_path);
int WebDAVConfigured(void);
const char *WebDAVName(void);
const char *WebDAVError(void);
void WebDAVPlaybackCacheReady(void);
#ifdef __cplusplus
}
#endif
#endif

#ifndef __SCREENSHOT_SERVER_H__
#define __SCREENSHOT_SERVER_H__

class CustomLcdDisplay;

// 启动截图 HTTP 服务：GET http://<设备IP>:8080/shot.bmp 返回当前画面（1bpp BMP）
// 需要在网络连上（拿到 IP）之后调用；重复调用安全（只启动一次）
bool ScreenshotServerStart(CustomLcdDisplay *display);

#endif

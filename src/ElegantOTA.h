/**
_____ _                        _    ___ _____  _    
| ____| | ___  __ _  __ _ _ __ | |_ / _ \_   _|/ \   
|  _| | |/ _ \/ _` |/ _` | '_ \| __| | | || | / _ \  
| |___| |  __/ (_| | (_| | | | | |_| |_| || |/ ___ \ 
|_____|_|\___|\__, |\__,_|_| |_|\__|\___/ |_/_/   \_\
              |___/                                  
*/

/**
 * 
 * @name ElegantOTA
 * @author Ayush Sharma (ayush@softt.io)
 * @brief 
 * @version 3.0.0
 * @date 2023-08-30
 */

#ifndef ElegantOTA_h
#define ElegantOTA_h

#include "Arduino.h"
#include "stdlib_noniso.h"
#include "elop.h"

#ifndef ELEGANTOTA_USE_ASYNC_WEBSERVER
  #define ELEGANTOTA_USE_ASYNC_WEBSERVER 1
#endif

#ifndef ELEGANTOTA_DEBUG
  #define ELEGANTOTA_DEBUG 0
#endif

#ifndef UPDATE_DEBUG
  #define UPDATE_DEBUG 0
#endif

#if ELEGANTOTA_DEBUG
  #define ELEGANTOTA_DEBUG_MSG(x) Serial.printf("%s %s", "[ElegantOTA] ", x)
#else
  #define ELEGANTOTA_DEBUG_MSG(x)
#endif

#if defined(ESP8266)
  #include <functional>
  #include "FS.h"
  #include "LittleFS.h"
  #include "Updater.h"
  #include "StreamString.h"
  #if ELEGANTOTA_USE_ASYNC_WEBSERVER == 1
    #include "ESPAsyncTCP.h"
    #include "ESPAsyncWebServer.h"
    #define ELEGANTOTA_WEBSERVER AsyncWebServer
  #else
    #include "ESP8266WiFi.h"
    #include "WiFiClient.h"
    #include "ESP8266WebServer.h"
    #define ELEGANTOTA_WEBSERVER ESP8266WebServer
  #endif
  #define HARDWARE "ESP8266"
#elif defined(ESP32)
  #include <functional>
  #include "FS.h"
  #include "Update.h"
  #include "StreamString.h"
  #include "esp_partition.h"
  #if ELEGANTOTA_USE_ASYNC_WEBSERVER == 1
    #include "AsyncTCP.h"
    #include "ESPAsyncWebServer.h"
    #define ELEGANTOTA_WEBSERVER AsyncWebServer
  #else
    #include "WiFi.h"
    #include "WiFiClient.h"
    #include "WebServer.h"
    #define ELEGANTOTA_WEBSERVER WebServer
  #endif
  #define HARDWARE "ESP32"
#elif defined(TARGET_RP2040) || defined(TARGET_RP2350) || defined(PICO_RP2040) || defined(PICO_RP2350)
  #include <functional>
  #include "Arduino.h"
  #include "StreamString.h"
  #include "FS.h"
  #include "LittleFS.h"
  #include "Updater.h"
  #if ELEGANTOTA_USE_ASYNC_WEBSERVER == 1
    #include "RPAsyncTCP.h"
    #include "ESPAsyncWebServer.h"
    #define ELEGANTOTA_WEBSERVER AsyncWebServer
  #else
    #include "WiFiClient.h"
    #include "WiFiServer.h"
    #include "WebServer.h"
    #define ELEGANTOTA_WEBSERVER WebServer
  #endif

  #if defined(TARGET_RP2040) || defined(PICO_RP2040)
    #define HARDWARE              "RP2040"
  #elif defined(TARGET_RP2350) || defined(PICO_RP2350)
    #define HARDWARE              "RP2350"
  #endif
  extern uint8_t _FS_start;
  extern uint8_t _FS_end;
#endif

enum OTA_Mode {
    OTA_MODE_FIRMWARE = 0,
    OTA_MODE_FILESYSTEM = 1
};

class ElegantOTAClass{
  public:
    /** Maximum accepted username or password length, excluding its NUL byte. */
    static const size_t MAX_AUTH_LENGTH = 64;
    /** Maximum Update error text retained for an HTTP error response. */
    static const size_t MAX_UPDATE_ERROR_LENGTH = 128;

    ElegantOTAClass();

    void begin(ELEGANTOTA_WEBSERVER *server, const char * username = "", const char * password = "");

    /**
     * Configure HTTP Basic authentication.
     *
     * Both credentials must be at most MAX_AUTH_LENGTH bytes. Supplying one
     * empty credential or an overlong credential returns false and rejects all
     * protected routes until clearAuth() or a valid setAuth() call is made;
     * credentials are never silently truncated.
     */
    bool setAuth(const char * username, const char * password);
    void clearAuth();
    void setAutoReboot(bool enable);
    void loop();

    void onStart(std::function<void()> callable);
    void onProgress(std::function<void(size_t current, size_t final)> callable);
    void onEnd(std::function<void(bool success)> callable);
    
  private:
    ELEGANTOTA_WEBSERVER *_server;

    bool _authenticate = false;
    bool _auth_configuration_invalid = false;
    char _username[MAX_AUTH_LENGTH + 1] = {};
    char _password[MAX_AUTH_LENGTH + 1] = {};
    size_t _username_length = 0;
    size_t _password_length = 0;

    bool _auto_reboot = true;
    bool _reboot = false;
    unsigned long _reboot_request_millis = 0;

    char _update_error[MAX_UPDATE_ERROR_LENGTH + 1] = {};
    size_t _update_error_length = 0;
    unsigned long _current_progress_size;

    std::function<void()> preUpdateCallback = NULL;
    std::function<void(size_t current, size_t final)> progressUpdateCallback = NULL;
    std::function<void(bool success)> postUpdateCallback = NULL;

    // Shared helpers
    void _registerRoutes();
    size_t _partitionSize(OTA_Mode mode);
    bool _beginUpdate(OTA_Mode mode);
    void _abortUpdate();
    static bool _copyMD5(const char * hash, char * out);
    static bool _copyBounded(const char * source, char * destination, size_t capacity, size_t &length);
    const char * _updateErrorMessage() const;
    void _captureUpdateError();
    void _buildMetadata(char * out, size_t len);
};

extern ElegantOTAClass ElegantOTA;
#endif

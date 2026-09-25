#include "ElegantOTA.h"

/**
 * The portal talks to the device over a handful of small routes. Everything
 * below is written once and expanded for whichever webserver the sketch is
 * compiled against, so the two builds cannot drift apart.
 */
#if ELEGANTOTA_USE_ASYNC_WEBSERVER == 1
  // AsyncWebServer retains these callbacks. Capture only the instance pointer
  // they need, rather than an implicit reference to this stack frame.
  #define EOTA_ROUTE(...)   [this](AsyncWebServerRequest *request) __VA_ARGS__
  #define EOTA_GUARD()      if (_auth_configuration_invalid || (_authenticate && !request->authenticate(_username, _password))) { request->requestAuthentication(); return; }
  #define EOTA_HAS(n)       request->hasParam(n)
  #define EOTA_ARG(n)       request->getParam(n)->value()
  #define EOTA_SEND(c,t,b)  request->send((c), (t), (b))
#else
  #define EOTA_ROUTE(...)   [this]() __VA_ARGS__
  #define EOTA_GUARD()      if (_auth_configuration_invalid || (_authenticate && !_server->authenticate(_username, _password))) { _server->requestAuthentication(); return; }
  #define EOTA_HAS(n)       _server->hasArg(n)
  #define EOTA_ARG(n)       _server->arg(n)
  #define EOTA_SEND(c,t,b)  _server->send((c), (t), (b))
#endif

// Route registration is a one-time allocation in ESPAsyncWebServer. Keep the
// measurement behind the normal debug switch so production builds pay no
// code-size or runtime cost. The delta is from the preceding registration (or
// from entry to _registerRoutes for /update).
#if ELEGANTOTA_USE_ASYNC_WEBSERVER == 1 && ELEGANTOTA_DEBUG && (defined(ESP8266) || defined(ESP32))
  #define EOTA_ASYNC_ROUTE_HEAP_BEGIN() uint32_t eota_route_heap = ESP.getFreeHeap()
  #define EOTA_ASYNC_ROUTE_HEAP_DELTA(route) do { \
    const uint32_t eota_free_heap = ESP.getFreeHeap(); \
    Serial.printf("[ElegantOTA] async route %s: free_heap=%lu delta=%ld\\n", (route), (unsigned long)eota_free_heap, (long)eota_free_heap - (long)eota_route_heap); \
    eota_route_heap = eota_free_heap; \
  } while (0)
#else
  #define EOTA_ASYNC_ROUTE_HEAP_BEGIN()
  #define EOTA_ASYNC_ROUTE_HEAP_DELTA(route)
#endif

ElegantOTAClass::ElegantOTAClass(){}

// ---------------------------------------------------------------------------
// Flash geometry + Update wrapper
// ---------------------------------------------------------------------------

/**
 * How many bytes the target region can hold. Reported to the portal so it can
 * tell you a build is too large before a single byte is written. Returns 0
 * where the platform cannot answer without side effects.
 */
size_t ElegantOTAClass::_partitionSize(OTA_Mode mode) {
  #if defined(ESP8266)
    if (mode == OTA_MODE_FILESYSTEM) return ((size_t)FS_end - (size_t)FS_start);
    return (size_t)((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
  #elif defined(ESP32)
    if (mode == OTA_MODE_FILESYSTEM) {
      const esp_partition_t * part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
      return part ? (size_t)part->size : 0;
    }
    return (size_t)ESP.getFreeSketchSpace();
  #elif defined(TARGET_RP2040) || defined(TARGET_RP2350) || defined(PICO_RP2040) || defined(PICO_RP2350)
    // Both modes are bounded by the filesystem region: a firmware image is
    // staged there as a file before the bootloader copies it into place, and
    // the core's own limit is exactly this span. Reported without mounting
    // anything, so answering a metadata request stays free of side effects.
    (void)mode;
    return ((size_t)&_FS_end - (size_t)&_FS_start);
  #else
    (void)mode;
    return 0;
  #endif
}

/** Copy exactly 32 hexadecimal digest bytes into a NUL-terminated buffer. */
bool ElegantOTAClass::_copyMD5(const char * hash, char * out) {
  if (!hash || !out) return false;
  for (size_t len = 0; len < 32; len++) {
    const char c = hash[len];
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (!hex) return false;
    out[len] = c;
  }
  if (hash[32] != '\0') return false;
  out[32] = '\0';
  return true;
}

/** Copy a C string only when it fits, always leaving destination terminated. */
bool ElegantOTAClass::_copyBounded(const char * source, char * destination, size_t capacity, size_t &length) {
  length = 0;
  if (!destination || !capacity) return false;
  destination[0] = '\0';
  if (!source) return true;
  while (length + 1 < capacity && source[length]) {
    destination[length] = source[length];
    length++;
  }
  destination[length] = '\0';
  return source[length] == '\0';
}

/** Close a flash region we opened but are not going to write to. */
void ElegantOTAClass::_abortUpdate() {
  #if defined(ESP32)
    Update.abort();
  #else
    Update.end();
  #endif
}

void ElegantOTAClass::_captureUpdateError() {
  StreamString str;
  Update.printError(str);
  _copyBounded(str.c_str(), _update_error, sizeof(_update_error), _update_error_length);
  while (_update_error_length && (_update_error[_update_error_length - 1] == ' ' || _update_error[_update_error_length - 1] == '\t' || _update_error[_update_error_length - 1] == '\r' || _update_error[_update_error_length - 1] == '\n')) {
    _update_error[--_update_error_length] = '\0';
  }
  ELEGANTOTA_DEBUG_MSG(_updateErrorMessage());
}

const char * ElegantOTAClass::_updateErrorMessage() const {
  return _update_error_length ? _update_error : "Update failed";
}

bool ElegantOTAClass::_beginUpdate(OTA_Mode mode) {
  #if defined(ESP8266)
    uint32_t update_size = mode == OTA_MODE_FILESYSTEM ? ((size_t)FS_end - (size_t)FS_start) : ((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
    if (mode == OTA_MODE_FILESYSTEM) {
      close_all_fs();
    }
    Update.runAsync(true);
    if (!Update.begin(update_size, mode == OTA_MODE_FILESYSTEM ? U_FS : U_FLASH)) {
      ELEGANTOTA_DEBUG_MSG("Failed to start update process\n");
      _captureUpdateError();
      return false;
    }
  #elif defined(ESP32)
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, mode == OTA_MODE_FILESYSTEM ? U_SPIFFS : U_FLASH)) {
      ELEGANTOTA_DEBUG_MSG("Failed to start update process\n");
      _captureUpdateError();
      return false;
    }
  #elif defined(TARGET_RP2040) || defined(TARGET_RP2350) || defined(PICO_RP2040) || defined(PICO_RP2350)
    // Update.begin() takes the budget the write is allowed to use, not the
    // space that happens to be free right now. Passing free space meant a
    // second update was refused for want of room the moment a previously
    // staged image was still sitting in the filesystem — even though that
    // file is truncated before the new one is written.
    uint32_t update_size = _partitionSize(mode);
    if (mode == OTA_MODE_FILESYSTEM) {
      LittleFS.end();
    }
    if (!Update.begin(update_size, mode == OTA_MODE_FILESYSTEM ? U_FS : U_FLASH)) {
      ELEGANTOTA_DEBUG_MSG("Failed to start update process\n");
      _captureUpdateError();
      return false;
    }
  #else
    (void)mode;
    return false;
  #endif
  return true;
}

void ElegantOTAClass::_buildMetadata(char * out, size_t len) {
  // Keys are kept short because this JSON crosses a very small pipe:
  // hw  chip family        ar  reboots itself after a successful write
  // fwa firmware space     fsa filesystem space (bytes, 0 = unknown)
  // fwu size of the build currently running (bytes, 0 = unknown)
  #if defined(ESP8266) || defined(ESP32)
    unsigned long running_size = (unsigned long)ESP.getSketchSize();
  #else
    unsigned long running_size = 0;
  #endif

  snprintf(out, len,
    "{\"hw\":\"%s\",\"ar\":%s,\"fwa\":%lu,\"fsa\":%lu,\"fwu\":%lu}",
    HARDWARE,
    _auto_reboot ? "true" : "false",
    (unsigned long)_partitionSize(OTA_MODE_FIRMWARE),
    (unsigned long)_partitionSize(OTA_MODE_FILESYSTEM),
    running_size
  );
}

// ---------------------------------------------------------------------------
// Routes
// ---------------------------------------------------------------------------

void ElegantOTAClass::begin(ELEGANTOTA_WEBSERVER *server, const char * username, const char * password){
  _server = server;

  setAuth(username, password);

  #if defined(TARGET_RP2040) || defined(TARGET_RP2350) || defined(PICO_RP2040) || defined(PICO_RP2350)
    if (!__isPicoW) {
      ELEGANTOTA_DEBUG_MSG("RP2040: Not a Pico W, skipping OTA setup\n");
      return;
    }
  #endif

  _registerRoutes();
}

void ElegantOTAClass::_registerRoutes(){
  EOTA_ASYNC_ROUTE_HEAP_BEGIN();
  // Portal
  #if ELEGANTOTA_USE_ASYNC_WEBSERVER == 1
    _server->on("/update", HTTP_GET, [this](AsyncWebServerRequest *request){
      if(_auth_configuration_invalid || (_authenticate && !request->authenticate(_username, _password))){
        return request->requestAuthentication();
      }
      #if defined(ASYNCWEBSERVER_VERSION) && ASYNCWEBSERVER_VERSION_MAJOR > 2  // This means we are using recommended fork of AsyncWebServer
        AsyncWebServerResponse *response = request->beginResponse(200, "text/html", ELEGANT_HTML, sizeof(ELEGANT_HTML));
      #else
        AsyncWebServerResponse *response = request->beginResponse_P(200, "text/html", ELEGANT_HTML, sizeof(ELEGANT_HTML));
      #endif
      response->addHeader("Content-Encoding", "gzip");
      request->send(response);
    });
    EOTA_ASYNC_ROUTE_HEAP_DELTA("/update");
  #else
    _server->on("/update", HTTP_GET, [this](){
      if (_auth_configuration_invalid || (_authenticate && !_server->authenticate(_username, _password))) {
        return _server->requestAuthentication();
      }
      _server->sendHeader("Content-Encoding", "gzip");
      _server->send_P(200, "text/html", (const char*)ELEGANT_HTML, sizeof(ELEGANT_HTML));
    });
  #endif

  // What the portal needs to draw itself: chip family and flash geometry
  _server->on("/ota/metadata", HTTP_GET, EOTA_ROUTE({
    char json[192];
    _buildMetadata(json, sizeof(json));
    EOTA_SEND(200, "application/json", json);
  }));
  EOTA_ASYNC_ROUTE_HEAP_DELTA("/ota/metadata");

  // Open the flash region ahead of an upload
  _server->on("/ota/start", HTTP_GET, EOTA_ROUTE({
    EOTA_GUARD();

    OTA_Mode mode = OTA_MODE_FIRMWARE;
    if (EOTA_HAS("mode") && EOTA_ARG("mode") == "fs") {
      ELEGANTOTA_DEBUG_MSG("OTA Mode: Filesystem\n");
      mode = OTA_MODE_FILESYSTEM;
    } else {
      ELEGANTOTA_DEBUG_MSG("OTA Mode: Firmware\n");
    }

    #if UPDATE_DEBUG == 1
      // Serial output must be active to see the callback serial prints
      Serial.setDebugOutput(true);
    #endif

    // Check the digest before opening anything. Rejecting it afterwards would
    // leave the flash region open, and every later update would be refused
    // because one is still running.
    char hash[33];
    bool has_hash = EOTA_HAS("hash");
    if (has_hash) {
      #if ELEGANTOTA_USE_ASYNC_WEBSERVER == 1
        // AsyncWebParameter owns its value for this request. Copy and validate
        // it immediately so no Arduino String is retained by this route.
        if (!_copyMD5(request->getParam("hash")->value().c_str(), hash)) {
      #else
        if (!_copyMD5(_server->arg("hash").c_str(), hash)) {
      #endif
        ELEGANTOTA_DEBUG_MSG("ERROR: MD5 hash not valid\n");
        EOTA_SEND(400, "text/plain", "That MD5 digest is not valid");
        return;
      }
    }

    // Pre-OTA update callback
    if (_pre_update_callback != NULL) {
      _pre_update_callback(_pre_update_context);
    }
    #if ELEGANTOTA_ENABLE_STD_FUNCTION_CALLBACKS
      else if (_legacy_pre_update_callback != NULL) {
        _legacy_pre_update_callback();
      }
    #endif

    if (!_beginUpdate(mode)) {
      EOTA_SEND(400, "text/plain", _updateErrorMessage());
      return;
    }

    // Update.begin() clears any previously set digest, so the expected hash
    // has to be handed over afterwards to actually be checked.
    if (has_hash && !Update.setMD5(hash)) {
      ELEGANTOTA_DEBUG_MSG("ERROR: MD5 hash rejected by Update\n");
      _abortUpdate();
      EOTA_SEND(400, "text/plain", "That MD5 digest is not valid");
      return;
    }

    EOTA_SEND(200, "text/plain", "OK");
  }));
  EOTA_ASYNC_ROUTE_HEAP_DELTA("/ota/start");

  // Browser upload
  #if ELEGANTOTA_USE_ASYNC_WEBSERVER == 1
    _server->on("/ota/upload", HTTP_POST, [this](AsyncWebServerRequest *request) {
        if(_auth_configuration_invalid || (_authenticate && !request->authenticate(_username, _password))){
          return request->requestAuthentication();
        }
        // Post-OTA update callback
        if (_post_update_callback != NULL) {
          _post_update_callback(_post_update_context, !Update.hasError());
        }
        #if ELEGANTOTA_ENABLE_STD_FUNCTION_CALLBACKS
          else if (_legacy_post_update_callback != NULL) {
            _legacy_post_update_callback(!Update.hasError());
          }
        #endif
        AsyncWebServerResponse *response = request->beginResponse((Update.hasError()) ? 400 : 200, "text/plain", (Update.hasError()) ? _updateErrorMessage() : "OK");
        request->send(response);
        // Set reboot flag
        if (!Update.hasError()) {
          if (_auto_reboot) {
            _reboot_request_millis = millis();
            _reboot = true;
          }
        }
    }, [this](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        //Upload handler chunks in data
        if(_auth_configuration_invalid || _authenticate){
            if(_auth_configuration_invalid || !request->authenticate(_username, _password)){
                return request->requestAuthentication();
            }
        }

        if (!index) {
          // Reset progress size on first frame
          _current_progress_size = 0;
        }

        // Write chunked data to the free sketch space
        if(len){
            if (Update.write(data, len) != len) {
                return request->send(400, "text/plain", "Failed to write chunked data to free space");
            }
            _current_progress_size += len;
            // Progress update callback
            if (_progress_update_callback != NULL) {
              _progress_update_callback(_progress_update_context, _current_progress_size, request->contentLength());
            }
            #if ELEGANTOTA_ENABLE_STD_FUNCTION_CALLBACKS
              else if (_legacy_progress_update_callback != NULL) {
                _legacy_progress_update_callback(_current_progress_size, request->contentLength());
              }
            #endif
        }

        if (final) { // if the final flag is set then this is the last frame of data
            if (!Update.end(true)) { //true to set the size to the current progress
                _captureUpdateError();
            }
        }else{
            return;
        }
    });
    EOTA_ASYNC_ROUTE_HEAP_DELTA("/ota/upload");
  #else
    _server->on("/ota/upload", HTTP_POST, [this](){
      if (_auth_configuration_invalid || (_authenticate && !_server->authenticate(_username, _password))) {
        return _server->requestAuthentication();
      }
      // Post-OTA update callback
      if (_post_update_callback != NULL) {
        _post_update_callback(_post_update_context, !Update.hasError());
      }
      #if ELEGANTOTA_ENABLE_STD_FUNCTION_CALLBACKS
        else if (_legacy_post_update_callback != NULL) {
          _legacy_post_update_callback(!Update.hasError());
        }
      #endif
      _server->send((Update.hasError()) ? 400 : 200, "text/plain", (Update.hasError()) ? _updateErrorMessage() : "OK");
      // Set reboot flag
      if (!Update.hasError()) {
        if (_auto_reboot) {
          _reboot_request_millis = millis();
          _reboot = true;
        }
      }
    }, [this](){
      // Actual OTA Download
      HTTPUpload& upload = _server->upload();
      if (upload.status == UPLOAD_FILE_START) {
        // Check authentication
        if (_auth_configuration_invalid || (_authenticate && !_server->authenticate(_username, _password))) {
          ELEGANTOTA_DEBUG_MSG("Authentication Failed on UPLOAD_FILE_START\n");
          return;
        }
        ELEGANTOTA_DEBUG_MSG(String("Update Received: "+upload.filename+"\n").c_str());
        _current_progress_size = 0;
      } else if (upload.status == UPLOAD_FILE_WRITE) {
          if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            #if UPDATE_DEBUG == 1
              Update.printError(Serial);
            #endif
          }

          _current_progress_size += upload.currentSize;
          // Progress update callback
          if (_progress_update_callback != NULL) {
            _progress_update_callback(_progress_update_context, _current_progress_size, upload.totalSize);
          }
          #if ELEGANTOTA_ENABLE_STD_FUNCTION_CALLBACKS
            else if (_legacy_progress_update_callback != NULL) {
              _legacy_progress_update_callback(_current_progress_size, upload.totalSize);
            }
          #endif
      } else if (upload.status == UPLOAD_FILE_END) {
          if (Update.end(true)) {
              ELEGANTOTA_DEBUG_MSG(String("Update Success: "+String(upload.totalSize)+"\n").c_str());
          } else {
              ELEGANTOTA_DEBUG_MSG("[!] Update Failed\n");
              _captureUpdateError();
          }

          #if UPDATE_DEBUG == 1
            Serial.setDebugOutput(false);
          #endif
      } else {
        ELEGANTOTA_DEBUG_MSG(String("Update Failed Unexpectedly (likely broken connection): status="+String(upload.status)+"\n").c_str());
      }
    });
  #endif
}

#undef EOTA_ASYNC_ROUTE_HEAP_BEGIN
#undef EOTA_ASYNC_ROUTE_HEAP_DELTA

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

bool ElegantOTAClass::setAuth(const char * username, const char * password){
  char copied_username[MAX_AUTH_LENGTH + 1] = {};
  char copied_password[MAX_AUTH_LENGTH + 1] = {};
  size_t username_length;
  size_t password_length;
  const bool username_fits = _copyBounded(username, copied_username, sizeof(copied_username), username_length);
  const bool password_fits = _copyBounded(password, copied_password, sizeof(copied_password), password_length);

  if (!username_fits || !password_fits || (username_length == 0) != (password_length == 0)) {
    memset(_username, 0, sizeof(_username));
    memset(_password, 0, sizeof(_password));
    _username_length = 0;
    _password_length = 0;
    _authenticate = false;
    _auth_configuration_invalid = true;
    ELEGANTOTA_DEBUG_MSG("ERROR: Invalid authentication credentials\n");
    return false;
  }

  memset(_username, 0, sizeof(_username));
  memset(_password, 0, sizeof(_password));
  memcpy(_username, copied_username, username_length + 1);
  memcpy(_password, copied_password, password_length + 1);
  _username_length = username_length;
  _password_length = password_length;
  _authenticate = username_length != 0;
  _auth_configuration_invalid = false;
  return true;
}

void ElegantOTAClass::clearAuth(){
  memset(_username, 0, sizeof(_username));
  memset(_password, 0, sizeof(_password));
  _username_length = 0;
  _password_length = 0;
  _authenticate = false;
  _auth_configuration_invalid = false;
}

void ElegantOTAClass::setAutoReboot(bool enable){
  _auto_reboot = enable;
}

void ElegantOTAClass::loop() {
  // Check if 2 seconds have passed since _reboot_request_millis was set
  if (_reboot && millis() - _reboot_request_millis > 2000) {
    ELEGANTOTA_DEBUG_MSG("Rebooting...\n");
    #if defined(ESP8266) || defined(ESP32)
      ESP.restart();
    #elif defined(TARGET_RP2040) || defined(TARGET_RP2350) || defined(PICO_RP2040) || defined(PICO_RP2350)
      rp2040.reboot();
    #endif
    _reboot = false;
  }
}

void ElegantOTAClass::onStart(StartCallback callback, void * context){
    _pre_update_callback = callback;
    _pre_update_context = context;
    #if ELEGANTOTA_ENABLE_STD_FUNCTION_CALLBACKS
      _legacy_pre_update_callback = NULL;
    #endif
}

void ElegantOTAClass::onProgress(ProgressCallback callback, void * context){
    _progress_update_callback = callback;
    _progress_update_context = context;
    #if ELEGANTOTA_ENABLE_STD_FUNCTION_CALLBACKS
      _legacy_progress_update_callback = NULL;
    #endif
}

void ElegantOTAClass::onEnd(EndCallback callback, void * context){
    _post_update_callback = callback;
    _post_update_context = context;
    #if ELEGANTOTA_ENABLE_STD_FUNCTION_CALLBACKS
      _legacy_post_update_callback = NULL;
    #endif
}

#if ELEGANTOTA_ENABLE_STD_FUNCTION_CALLBACKS
void ElegantOTAClass::onStart(std::function<void()> callable){
    _pre_update_callback = NULL;
    _pre_update_context = NULL;
    _legacy_pre_update_callback = callable;
}

void ElegantOTAClass::onProgress(std::function<void(size_t current, size_t final)> callable){
    _progress_update_callback = NULL;
    _progress_update_context = NULL;
    _legacy_progress_update_callback = callable;
}

void ElegantOTAClass::onEnd(std::function<void(bool success)> callable){
    _post_update_callback = NULL;
    _post_update_context = NULL;
    _legacy_post_update_callback = callable;
}
#endif

ElegantOTAClass ElegantOTA;

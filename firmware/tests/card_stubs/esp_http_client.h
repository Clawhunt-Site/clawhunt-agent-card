// The part of esp_http_client that main/cloud.c uses. Requests go to the test (card_host.h).
#pragma once

#include "esp_err.h"

#include <stdbool.h>

typedef struct esp_http_client *esp_http_client_handle_t;

// The client's own errors, with ESP-IDF's numbers. A request that ends in a 401 fails with ESP_FAIL.
#define ESP_ERR_HTTP_BASE              0x7000
#define ESP_ERR_HTTP_CONNECT           (ESP_ERR_HTTP_BASE + 2)   // the connection could not be opened
#define ESP_ERR_HTTP_WRITE_DATA        (ESP_ERR_HTTP_BASE + 3)   // the request could not be written
#define ESP_ERR_HTTP_FETCH_HEADER      (ESP_ERR_HTTP_BASE + 4)   // reading the answer's headers failed
#define ESP_ERR_HTTP_EAGAIN            (ESP_ERR_HTTP_BASE + 7)   // nothing came before the timeout
#define ESP_ERR_HTTP_CONNECTION_CLOSED (ESP_ERR_HTTP_BASE + 8)   // the read met the end of the connection
#define ESP_ERR_HTTP_INCOMPLETE_DATA   (ESP_ERR_HTTP_BASE + 12)  // the answer broke off in its body

typedef enum {
    HTTP_EVENT_ERROR = 0,       // a request failed
    HTTP_EVENT_ON_CONNECTED,    // the client opened a connection
    HTTP_EVENT_HEADERS_SENT,    // the request's headers are written
    HTTP_EVENT_ON_HEADER,       // a header of the answer came
    HTTP_EVENT_ON_DATA,         // a piece of the answer's body came
    HTTP_EVENT_ON_FINISH,       // the answer is complete
    HTTP_EVENT_DISCONNECTED,    // the client closed its connection
} esp_http_client_event_id_t;

typedef struct {
    esp_http_client_event_id_t event_id;
    void *data;
    int data_len;
} esp_http_client_event_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t *evt);

typedef enum {
    HTTP_METHOD_GET = 0,
    HTTP_METHOD_POST,
} esp_http_client_method_t;

typedef struct {
    const char *url;
    esp_err_t (*crt_bundle_attach)(void *conf);
    int timeout_ms;
    const char *user_agent;
    http_event_handle_cb event_handler;
    int buffer_size;
    int buffer_size_tx;
    bool disable_auto_redirect;
    int max_authorization_retries;
} esp_http_client_config_t;

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char *url);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client, esp_http_client_method_t method);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char *key, const char *value);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char *key);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char *data, int len);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
int esp_http_client_get_status_code(esp_http_client_handle_t client);

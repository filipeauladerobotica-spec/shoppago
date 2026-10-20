#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <openssl/hmac.h>
#include <ctype.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <json-c/json.h>
#include <curl/curl.h>

#define PORT 8080
#define REQUEST_CAPACITY 65536
#define SESSION_BYTES 32
#define SESSION_MAX_AGE (60 * 60 * 24 * 30)
#define PBKDF2_ITERATIONS 210000
#define OAUTH_STATE_BYTES 32
#define OAUTH_STATE_MAX_AGE 600
#define OAUTH_RESPONSE_CAPACITY 32768

static sqlite3 *database;
static int secure_cookie;

typedef struct {
    char data[OAUTH_RESPONSE_CAPACITY];
    size_t length;
} HttpResponse;

static struct json_object *parse_json(const char *data, size_t length);
static int valid_address_field(const char *value, size_t minimum, size_t maximum);

static int send_all(int client, const char *data, size_t length) {
    while (length > 0) {
        ssize_t sent = send(client, data, length, 0);
        if (sent <= 0) return -1;
        data += sent;
        length -= (size_t)sent;
    }
    return 0;
}

static size_t receive_http_data(char *data, size_t size, size_t count, void *context) {
    HttpResponse *response = context;
    size_t length = size * count;
    if (length > sizeof(response->data) - response->length - 1) return 0;
    memcpy(response->data + response->length, data, length);
    response->length += length;
    response->data[response->length] = '\0';
    return length;
}

static int http_get_json(const char *url, const char *authorization,
                         struct json_object **result) {
    CURL *curl = curl_easy_init();
    if (curl == NULL) return -1;
    HttpResponse response = {{0}, 0};
    struct curl_slist *headers = NULL;
    if (authorization != NULL) {
        headers = curl_slist_append(headers, authorization);
        if (headers == NULL) {
            curl_easy_cleanup(curl);
            return -1;
        }
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_http_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    if (headers != NULL) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    CURLcode result_code = curl_easy_perform(curl);
    long status = 0;
    if (result_code == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result_code != CURLE_OK || status != 200) return -1;
    *result = parse_json(response.data, response.length);
    return *result == NULL ? -1 : 0;
}

static int http_post_form(const char *url, const char *form, struct json_object **result) {
    CURL *curl = curl_easy_init();
    if (curl == NULL) return -1;
    HttpResponse response = {{0}, 0};
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/x-www-form-urlencoded");
    if (headers == NULL) {
        curl_easy_cleanup(curl);
        return -1;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, form);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(form));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_http_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    CURLcode result_code = curl_easy_perform(curl);
    long status = 0;
    if (result_code == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result_code != CURLE_OK || status != 200) return -1;
    *result = parse_json(response.data, response.length);
    return *result == NULL ? -1 : 0;
}

static int http_post_json(const char *url, const char *authorization,
                          struct json_object *body, struct json_object **result) {
    CURL *curl = curl_easy_init();
    if (curl == NULL) return -1;
    HttpResponse response = {{0}, 0};
    struct curl_slist *headers = curl_slist_append(NULL, "Content-Type: application/json");
    if (headers == NULL) {
        curl_easy_cleanup(curl);
        return -1;
    }
    char auth_header[4096];
    if (authorization != NULL) {
        int length = snprintf(auth_header, sizeof(auth_header), "Authorization: Bearer %s",
                              authorization);
        if (length < 0 || (size_t)length >= sizeof(auth_header)) {
            OPENSSL_cleanse(auth_header, sizeof(auth_header));
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);
            return -1;
        }
        struct curl_slist *updated = curl_slist_append(headers, auth_header);
        OPENSSL_cleanse(auth_header, sizeof(auth_header));
        curl_slist_free_all(headers);
        if (updated == NULL) {
            curl_easy_cleanup(curl);
            return -1;
        }
        headers = updated;
    }
    const char *json = json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(json));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_http_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    CURLcode result_code = curl_easy_perform(curl);
    long status = 0;
    if (result_code == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    if (result_code != CURLE_OK || status < 200 || status >= 300) return -1;
    *result = parse_json(response.data, response.length);
    return *result == NULL ? -1 : 0;
}

static int mercadopago_configured(void) {
    const char *token = getenv("MP_ACCESS_TOKEN");
    const char *webhook_secret = getenv("MP_WEBHOOK_SECRET");
    const char *base_url = getenv("PUBLIC_BASE_URL");
    if (base_url == NULL || strncmp(base_url, "https://", 8) != 0 ||
        strlen(base_url) >= 512 || base_url[strlen(base_url) - 1] == '/') return 0;
    for (const char *cursor = base_url; *cursor != '\0'; cursor++) {
        if (isspace((unsigned char)*cursor) || *cursor == '?' || *cursor == '#') return 0;
    }
    return token != NULL && token[0] != '\0' &&
           webhook_secret != NULL && webhook_secret[0] != '\0' &&
           base_url[8] != '\0';
}

#define RESPOND_TEXT(client, status, message) \
    respond(client, status, "text/plain", message, sizeof(message) - 1, NULL)

static void respond(int client, const char *status, const char *content_type,
                    const char *body, size_t body_length, const char *cookie) {
    char header[1024];
    int written = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s; charset=utf-8\r\n"
        "Content-Length: %zu\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
        "Referrer-Policy: strict-origin-when-cross-origin\r\n"
        "Content-Security-Policy: default-src 'self'; img-src 'self' https:; "
        "style-src 'self' https://fonts.googleapis.com; "
        "font-src 'self' https://fonts.gstatic.com; script-src 'self'; "
        "connect-src 'self'; object-src 'none'; base-uri 'self'; frame-ancestors 'none'\r\n"
        "%sConnection: close\r\n\r\n",
        status, content_type, body_length, cookie == NULL ? "" : cookie);
    if (written > 0 && (size_t)written < sizeof(header)) {
        send_all(client, header, (size_t)written);
        send_all(client, body, body_length);
    }
}

static void respond_json(int client, const char *status, struct json_object *body,
                         const char *cookie) {
    const char *json = json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN);
    respond(client, status, "application/json", json, strlen(json), cookie);
    json_object_put(body);
}

static void respond_redirect(int client, const char *location, const char *cookie) {
    char header[8192];
    int written = snprintf(header, sizeof(header),
        "HTTP/1.1 303 See Other\r\nLocation: %s\r\nCache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n"
        "Content-Security-Policy: default-src 'none'; frame-ancestors 'none'\r\n"
        "%sContent-Length: 0\r\nConnection: close\r\n\r\n",
        location, cookie == NULL ? "" : cookie);
    if (written > 0 && (size_t)written < sizeof(header)) send_all(client, header, (size_t)written);
}

static void respond_error(int client, const char *status, const char *message) {
    struct json_object *body = json_object_new_object();
    json_object_object_add(body, "error", json_object_new_string(message));
    respond_json(client, status, body, NULL);
}

static int execute_sql(const char *sql) {
    char *error = NULL;
    int result = sqlite3_exec(database, sql, NULL, NULL, &error);
    if (result != SQLITE_OK) {
        fprintf(stderr, "SQLite: %s\n", error == NULL ? sqlite3_errmsg(database) : error);
        sqlite3_free(error);
        return -1;
    }
    return 0;
}

static int ensure_column(const char *table, const char *column, const char *definition) {
    char query[128];
    int length = snprintf(query, sizeof(query), "PRAGMA table_info(%s)", table);
    if (length < 0 || (size_t)length >= sizeof(query)) return -1;
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, query, -1, &statement, NULL) != SQLITE_OK) return -1;
    int found = 0;
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(statement, 1);
        if (name != NULL && strcmp(name, column) == 0) found = 1;
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) return -1;
    if (found) return 0;
    char alter[256];
    length = snprintf(alter, sizeof(alter), "ALTER TABLE %s ADD COLUMN %s %s",
                      table, column, definition);
    if (length < 0 || (size_t)length >= sizeof(alter)) return -1;
    return execute_sql(alter);
}

static int initialize_database(void) {
    const char *path = getenv("VITRINE_DB_PATH");
    if (path == NULL || path[0] == '\0') path = "/data/vitrine.db";
    if (sqlite3_open(path, &database) != SQLITE_OK) {
        fprintf(stderr, "Não foi possível abrir o banco de dados: %s\n", sqlite3_errmsg(database));
        return -1;
    }
    sqlite3_busy_timeout(database, 5000);
    if (execute_sql("PRAGMA foreign_keys = ON; PRAGMA journal_mode = WAL;") != 0) return -1;
    if (execute_sql(
        "CREATE TABLE IF NOT EXISTS users ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,"
        "email TEXT NOT NULL COLLATE NOCASE UNIQUE,"
        "password_salt TEXT NOT NULL,"
        "password_hash TEXT NOT NULL,"
        "google_sub TEXT,"
        "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS oauth_states ("
        "state_hash TEXT PRIMARY KEY,"
        "expires_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS sessions ("
        "token_hash TEXT PRIMARY KEY,"
        "user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
        "expires_at INTEGER NOT NULL);"
        "CREATE TABLE IF NOT EXISTS products ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "seller_id INTEGER NOT NULL REFERENCES users(id),"
        "name TEXT NOT NULL,"
        "description TEXT NOT NULL,"
        "category TEXT NOT NULL,"
        "price_cents INTEGER NOT NULL CHECK(price_cents > 0),"
        "shipping_cents INTEGER NOT NULL DEFAULT 0 CHECK(shipping_cents >= 0),"
        "image_url TEXT NOT NULL DEFAULT '',"
        "active INTEGER NOT NULL DEFAULT 1,"
        "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE INDEX IF NOT EXISTS products_active_created ON products(active, created_at DESC);"
        "CREATE TABLE IF NOT EXISTS cart_items ("
        "user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
        "product_id INTEGER NOT NULL REFERENCES products(id),"
        "quantity INTEGER NOT NULL CHECK(quantity BETWEEN 1 AND 20),"
        "PRIMARY KEY(user_id, product_id));"
        "CREATE TABLE IF NOT EXISTS orders ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "buyer_id INTEGER NOT NULL REFERENCES users(id),"
        "status TEXT NOT NULL DEFAULT 'awaiting_payment',"
        "total_cents INTEGER NOT NULL,"
        "shipping_cents INTEGER NOT NULL DEFAULT 0,"
        "recipient_name TEXT NOT NULL DEFAULT '',"
        "recipient_phone TEXT NOT NULL DEFAULT '',"
        "postal_code TEXT NOT NULL DEFAULT '',"
        "state TEXT NOT NULL DEFAULT '',"
        "city TEXT NOT NULL DEFAULT '',"
        "street TEXT NOT NULL DEFAULT '',"
        "address_number TEXT NOT NULL DEFAULT '',"
        "complement TEXT NOT NULL DEFAULT '',"
        "external_reference TEXT UNIQUE,"
        "preference_id TEXT NOT NULL DEFAULT '',"
        "checkout_url TEXT NOT NULL DEFAULT '',"
        "payment_id TEXT NOT NULL DEFAULT '',"
        "payment_status TEXT NOT NULL DEFAULT 'pending',"
        "tracking_code TEXT NOT NULL DEFAULT '',"
        "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);"
        "CREATE TABLE IF NOT EXISTS order_items ("
        "order_id INTEGER NOT NULL REFERENCES orders(id) ON DELETE CASCADE,"
        "product_id INTEGER NOT NULL,"
        "seller_id INTEGER NOT NULL,"
        "product_name TEXT NOT NULL,"
        "unit_price_cents INTEGER NOT NULL,"
        "shipping_cents INTEGER NOT NULL DEFAULT 0,"
        "quantity INTEGER NOT NULL,"
        "PRIMARY KEY(order_id, product_id));"
        "CREATE TABLE IF NOT EXISTS order_shipments ("
        "order_id INTEGER NOT NULL REFERENCES orders(id) ON DELETE CASCADE,"
        "seller_id INTEGER NOT NULL REFERENCES users(id),"
        "status TEXT NOT NULL DEFAULT 'awaiting_payment',"
        "tracking_code TEXT NOT NULL DEFAULT '',"
        "updated_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "PRIMARY KEY(order_id,seller_id));"
        "CREATE TABLE IF NOT EXISTS product_reviews ("
        "product_id INTEGER NOT NULL REFERENCES products(id) ON DELETE CASCADE,"
        "user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,"
        "rating INTEGER NOT NULL CHECK(rating BETWEEN 1 AND 5),"
        "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP,"
        "PRIMARY KEY(product_id,user_id));"
        "CREATE TABLE IF NOT EXISTS seller_profiles ("
        "user_id INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,"
        "birth_date TEXT NOT NULL,"
        "phone TEXT NOT NULL,"
        "interests TEXT NOT NULL,"
        "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP);") != 0) return -1;

    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, "PRAGMA table_info(users)", -1, &statement, NULL) != SQLITE_OK) {
        return -1;
    }
    int google_sub_exists = 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        const char *column = (const char *)sqlite3_column_text(statement, 1);
        if (column != NULL && strcmp(column, "google_sub") == 0) google_sub_exists = 1;
    }
    sqlite3_finalize(statement);
    if (!google_sub_exists && execute_sql("ALTER TABLE users ADD COLUMN google_sub TEXT") != 0) return -1;
    if (ensure_column("products", "shipping_cents", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column("orders", "shipping_cents", "INTEGER NOT NULL DEFAULT 0") != 0 ||
        ensure_column("orders", "recipient_name", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column("orders", "recipient_phone", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column("orders", "postal_code", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column("orders", "state", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column("orders", "city", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column("orders", "street", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column("orders", "address_number", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column("orders", "complement", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column("orders", "external_reference", "TEXT") != 0 ||
        ensure_column("orders", "preference_id", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column("orders", "checkout_url", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column("orders", "payment_id", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column("orders", "payment_status", "TEXT NOT NULL DEFAULT 'pending'") != 0 ||
        ensure_column("orders", "tracking_code", "TEXT NOT NULL DEFAULT ''") != 0 ||
        ensure_column("order_items", "shipping_cents", "INTEGER NOT NULL DEFAULT 0") != 0) return -1;
    return execute_sql("CREATE UNIQUE INDEX IF NOT EXISTS users_google_sub_unique ON users(google_sub) WHERE google_sub IS NOT NULL;"
                       "CREATE UNIQUE INDEX IF NOT EXISTS orders_external_reference_unique ON orders(external_reference) WHERE external_reference IS NOT NULL;"
                       "CREATE INDEX IF NOT EXISTS oauth_states_expiry ON oauth_states(expires_at);");
}

static struct json_object *parse_json(const char *data, size_t length) {
    struct json_tokener *tokener = json_tokener_new();
    if (tokener == NULL) return NULL;
    struct json_object *object = json_tokener_parse_ex(tokener, data, (int)length);
    enum json_tokener_error error = json_tokener_get_error(tokener);
    size_t parsed_length = json_tokener_get_parse_end(tokener);
    json_tokener_free(tokener);
    while (parsed_length < length && isspace((unsigned char)data[parsed_length])) parsed_length++;
    if (error != json_tokener_success || object == NULL ||
        parsed_length != length || json_object_get_type(object) != json_type_object) {
        if (object != NULL) json_object_put(object);
        return NULL;
    }
    return object;
}

static const char *json_string(struct json_object *object, const char *key) {
    struct json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) ||
        json_object_get_type(value) != json_type_string) return NULL;
    return json_object_get_string(value);
}

static int json_integer(struct json_object *object, const char *key, int64_t *result) {
    struct json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) ||
        json_object_get_type(value) != json_type_int) return -1;
    *result = json_object_get_int64(value);
    return 0;
}

static void bytes_to_hex(const unsigned char *bytes, size_t length, char *hex) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < length; i++) {
        hex[i * 2] = digits[bytes[i] >> 4];
        hex[i * 2 + 1] = digits[bytes[i] & 15];
    }
    hex[length * 2] = '\0';
}

static int password_hash(const char *password, const char *salt_hex, char *hash_hex) {
    unsigned char salt[16];
    unsigned char hash[32];
    for (size_t i = 0; i < sizeof(salt); i++) {
        unsigned int byte;
        if (sscanf(salt_hex + i * 2, "%2x", &byte) != 1) return -1;
        salt[i] = (unsigned char)byte;
    }
    if (PKCS5_PBKDF2_HMAC(password, (int)strlen(password), salt, sizeof(salt),
                          PBKDF2_ITERATIONS, EVP_sha256(), sizeof(hash), hash) != 1) return -1;
    bytes_to_hex(hash, sizeof(hash), hash_hex);
    OPENSSL_cleanse(hash, sizeof(hash));
    return 0;
}

static int store_session(int64_t user_id, char *cookie, size_t cookie_size) {
    unsigned char token[SESSION_BYTES];
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    char token_hex[SESSION_BYTES * 2 + 1];
    char digest_hex[EVP_MAX_MD_SIZE * 2 + 1];
    if (RAND_bytes(token, sizeof(token)) != 1 ||
        EVP_Digest(token, sizeof(token), digest, &digest_length, EVP_sha256(), NULL) != 1) {
        fprintf(stderr, "Não foi possível gerar uma sessão segura.\n");
        return -1;
    }
    bytes_to_hex(token, sizeof(token), token_hex);
    bytes_to_hex(digest, digest_length, digest_hex);

    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database,
            "INSERT INTO sessions(token_hash,user_id,expires_at) VALUES(?,?,?)",
            -1, &statement, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(statement, 1, digest_hex, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, user_id);
    sqlite3_bind_int64(statement, 3, (int64_t)time(NULL) + SESSION_MAX_AGE);
    int result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        fprintf(stderr, "Não foi possível criar a sessão: %s\n", sqlite3_errmsg(database));
        return -1;
    }
    snprintf(cookie, cookie_size,
             "Set-Cookie: vitrine_session=%s; Path=/; HttpOnly; SameSite=Lax; Max-Age=%d%s\r\n",
             token_hex, SESSION_MAX_AGE, secure_cookie ? "; Secure" : "");
    OPENSSL_cleanse(token, sizeof(token));
    return 0;
}

static int make_session(int client, int64_t user_id, const char *status) {
    char cookie[256];
    if (store_session(user_id, cookie, sizeof(cookie)) != 0) {
        respond_error(client, "500 Internal Server Error", "Não foi possível iniciar a sessão.");
        return -1;
    }
    respond(client, status, "application/json", "{\"ok\":true}", 11, cookie);
    return 0;
}

static int get_authenticated_user(const char *request, int64_t *user_id,
                                  char *name, size_t name_size, char *email, size_t email_size) {
    const char *headers = strstr(request, "\r\n");
    if (headers == NULL) return 0;
    const char *cookie_header = strstr(headers + 2, "\r\nCookie:");
    if (cookie_header == NULL) cookie_header = strstr(headers + 2, "\nCookie:");
    if (cookie_header == NULL) return 0;
    const char *token = strstr(cookie_header, "vitrine_session=");
    if (token == NULL) return 0;
    token += strlen("vitrine_session=");
    char token_hex[SESSION_BYTES * 2 + 1];
    size_t length = 0;
    while (length < SESSION_BYTES * 2 && isxdigit((unsigned char)token[length])) {
        token_hex[length] = token[length];
        length++;
    }
    if (length != SESSION_BYTES * 2) return 0;
    token_hex[length] = '\0';

    unsigned char token_bytes[SESSION_BYTES];
    for (size_t i = 0; i < SESSION_BYTES; i++) {
        unsigned int byte;
        if (sscanf(token_hex + i * 2, "%2x", &byte) != 1) return 0;
        token_bytes[i] = (unsigned char)byte;
    }
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    char digest_hex[EVP_MAX_MD_SIZE * 2 + 1];
    if (EVP_Digest(token_bytes, sizeof(token_bytes), digest, &digest_length, EVP_sha256(), NULL) != 1) return 0;
    bytes_to_hex(digest, digest_length, digest_hex);

    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database,
            "SELECT users.id,users.name,users.email FROM sessions "
            "JOIN users ON users.id=sessions.user_id WHERE sessions.token_hash=? AND sessions.expires_at>?",
            -1, &statement, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(statement, 1, digest_hex, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, (int64_t)time(NULL));
    int found = 0;
    if (sqlite3_step(statement) == SQLITE_ROW) {
        *user_id = sqlite3_column_int64(statement, 0);
        snprintf(name, name_size, "%s", (const char *)sqlite3_column_text(statement, 1));
        snprintf(email, email_size, "%s", (const char *)sqlite3_column_text(statement, 2));
        found = 1;
    }
    sqlite3_finalize(statement);
    OPENSSL_cleanse(token_bytes, sizeof(token_bytes));
    return found;
}

static struct json_object *user_json(int64_t user_id, const char *name, const char *email) {
    struct json_object *user = json_object_new_object();
    json_object_object_add(user, "id", json_object_new_int64(user_id));
    json_object_object_add(user, "name", json_object_new_string(name));
    json_object_object_add(user, "email", json_object_new_string(email));
    return user;
}

static int valid_email(const char *email) {
    size_t length = strlen(email);
    const char *at = strchr(email, '@');
    return length >= 5 && length <= 254 && at != NULL && at != email &&
           strchr(at + 1, '.') != NULL && strchr(email, ' ') == NULL;
}

static void handle_register(int client, struct json_object *body) {
    const char *name = json_string(body, "name");
    const char *email = json_string(body, "email");
    const char *password = json_string(body, "password");
    if (name == NULL || email == NULL || password == NULL || strlen(name) < 2 ||
        strlen(name) > 80 || !valid_email(email) || strlen(password) < 10 ||
        strlen(password) > 256) {
        respond_error(client, "400 Bad Request",
                      "Informe nome, e-mail válido e senha com pelo menos 10 caracteres.");
        return;
    }

    unsigned char salt[16];
    char salt_hex[33];
    char hash_hex[65];
    if (RAND_bytes(salt, sizeof(salt)) != 1) {
        respond_error(client, "500 Internal Server Error", "Não foi possível criar a conta.");
        return;
    }
    bytes_to_hex(salt, sizeof(salt), salt_hex);
    if (password_hash(password, salt_hex, hash_hex) != 0) {
        respond_error(client, "500 Internal Server Error", "Não foi possível proteger a senha.");
        return;
    }
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database,
            "INSERT INTO users(name,email,password_salt,password_hash) VALUES(?,?,?,?)",
            -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível criar a conta.");
        return;
    }
    sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, email, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, salt_hex, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, hash_hex, -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        if (result == SQLITE_CONSTRAINT) {
            respond_error(client, "409 Conflict", "Este e-mail já está cadastrado.");
        } else {
            fprintf(stderr, "Não foi possível registrar a conta: %s\n", sqlite3_errmsg(database));
            respond_error(client, "500 Internal Server Error", "Não foi possível criar a conta.");
        }
        return;
    }
    make_session(client, sqlite3_last_insert_rowid(database), "201 Created");
}

static void handle_login(int client, struct json_object *body) {
    const char *email = json_string(body, "email");
    const char *password = json_string(body, "password");
    if (email == NULL || password == NULL) {
        respond_error(client, "400 Bad Request", "Informe seu e-mail e sua senha.");
        return;
    }
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database,
            "SELECT id,password_salt,password_hash FROM users WHERE email=?",
            -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível verificar o acesso.");
        return;
    }
    sqlite3_bind_text(statement, 1, email, -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(statement);
    int64_t user_id = 0;
    int authenticated = 0;
    if (result == SQLITE_ROW) {
        char hash_hex[65];
        const char *salt = (const char *)sqlite3_column_text(statement, 1);
        const char *stored_hash = (const char *)sqlite3_column_text(statement, 2);
        if (password_hash(password, salt, hash_hex) == 0 &&
            CRYPTO_memcmp(hash_hex, stored_hash, 64) == 0) {
            user_id = sqlite3_column_int64(statement, 0);
            authenticated = 1;
        }
        OPENSSL_cleanse(hash_hex, sizeof(hash_hex));
    }
    sqlite3_finalize(statement);
    if (!authenticated) {
        respond_error(client, "401 Unauthorized", "E-mail ou senha incorretos.");
        return;
    }
    make_session(client, user_id, "200 OK");
}

static void handle_logout(int client, int64_t user_id, const char *request) {
    if (user_id > 0) {
        const char *token = strstr(request, "vitrine_session=");
        if (token != NULL) {
            token += strlen("vitrine_session=");
            unsigned char token_bytes[SESSION_BYTES];
            size_t length = 0;
            while (length < SESSION_BYTES * 2 && isxdigit((unsigned char)token[length])) length++;
            if (length == SESSION_BYTES * 2) {
                char token_hex[SESSION_BYTES * 2 + 1];
                char digest_hex[EVP_MAX_MD_SIZE * 2 + 1];
                unsigned char digest[EVP_MAX_MD_SIZE];
                unsigned int digest_length = 0;
                memcpy(token_hex, token, length);
                token_hex[length] = '\0';
                for (size_t i = 0; i < SESSION_BYTES; i++) {
                    unsigned int byte;
                    sscanf(token_hex + i * 2, "%2x", &byte);
                    token_bytes[i] = (unsigned char)byte;
                }
                if (EVP_Digest(token_bytes, sizeof(token_bytes), digest, &digest_length,
                               EVP_sha256(), NULL) == 1) {
                    bytes_to_hex(digest, digest_length, digest_hex);
                    sqlite3_stmt *statement = NULL;
                    if (sqlite3_prepare_v2(database, "DELETE FROM sessions WHERE token_hash=?",
                                           -1, &statement, NULL) != SQLITE_OK) {
                        respond_error(client, "500 Internal Server Error", "Não foi possível encerrar a sessão.");
                        OPENSSL_cleanse(token_bytes, sizeof(token_bytes));
                        return;
                    }
                    sqlite3_bind_text(statement, 1, digest_hex, -1, SQLITE_TRANSIENT);
                    int result = sqlite3_step(statement);
                    sqlite3_finalize(statement);
                    if (result != SQLITE_DONE) {
                        respond_error(client, "500 Internal Server Error", "Não foi possível encerrar a sessão.");
                        OPENSSL_cleanse(token_bytes, sizeof(token_bytes));
                        return;
                    }
                } else {
                    respond_error(client, "500 Internal Server Error", "Não foi possível encerrar a sessão.");
                    OPENSSL_cleanse(token_bytes, sizeof(token_bytes));
                    return;
                }
                OPENSSL_cleanse(token_bytes, sizeof(token_bytes));
            }
        }
    }
    char cookie[160];
    snprintf(cookie, sizeof(cookie),
             "Set-Cookie: vitrine_session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0%s\r\n",
             secure_cookie ? "; Secure" : "");
    respond(client, "200 OK", "application/json", "{\"ok\":true}", 11, cookie);
}

static struct json_object *product_json(sqlite3_stmt *statement) {
    struct json_object *product = json_object_new_object();
    json_object_object_add(product, "id", json_object_new_int64(sqlite3_column_int64(statement, 0)));
    json_object_object_add(product, "name", json_object_new_string((const char *)sqlite3_column_text(statement, 1)));
    json_object_object_add(product, "description", json_object_new_string((const char *)sqlite3_column_text(statement, 2)));
    json_object_object_add(product, "category", json_object_new_string((const char *)sqlite3_column_text(statement, 3)));
    json_object_object_add(product, "priceCents", json_object_new_int64(sqlite3_column_int64(statement, 4)));
    json_object_object_add(product, "imageUrl", json_object_new_string((const char *)sqlite3_column_text(statement, 5)));
    json_object_object_add(product, "sellerName", json_object_new_string((const char *)sqlite3_column_text(statement, 6)));
    json_object_object_add(product, "ratingAverage", json_object_new_double(sqlite3_column_double(statement, 7)));
    json_object_object_add(product, "ratingCount", json_object_new_int64(sqlite3_column_int64(statement, 8)));
    json_object_object_add(product, "shippingCents", json_object_new_int64(sqlite3_column_int64(statement, 9)));
    return product;
}

static void handle_products(int client, const char *search) {
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT products.id,products.name,products.description,products.category,"
        "products.price_cents,products.image_url,users.name,"
        "COALESCE((SELECT AVG(rating) FROM product_reviews WHERE product_id=products.id),0),"
        "(SELECT COUNT(*) FROM product_reviews WHERE product_id=products.id),"
        "products.shipping_cents FROM products "
        "JOIN users ON users.id=products.seller_id WHERE products.active=1 "
        "AND (?='' OR products.name LIKE ? OR products.description LIKE ? OR products.category LIKE ?) "
        "ORDER BY products.created_at DESC,products.id DESC LIMIT 100";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível carregar o catálogo.");
        return;
    }
    char pattern[520];
    snprintf(pattern, sizeof(pattern), "%%%.*s%%", 500, search);
    sqlite3_bind_text(statement, 1, search, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, pattern, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, pattern, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, pattern, -1, SQLITE_TRANSIENT);
    struct json_object *response = json_object_new_object();
    struct json_object *products = json_object_new_array();
    while (sqlite3_step(statement) == SQLITE_ROW) {
        json_object_array_add(products, product_json(statement));
    }
    sqlite3_finalize(statement);
    json_object_object_add(response, "products", products);
    respond_json(client, "200 OK", response, NULL);
}

static void handle_my_products(int client, int64_t user_id) {
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT products.id,products.name,products.description,products.category,"
        "products.price_cents,products.image_url,users.name,"
        "COALESCE((SELECT AVG(rating) FROM product_reviews WHERE product_id=products.id),0),"
        "(SELECT COUNT(*) FROM product_reviews WHERE product_id=products.id),"
        "products.shipping_cents FROM products "
        "JOIN users ON users.id=products.seller_id WHERE products.seller_id=? "
        "AND products.active=1 ORDER BY products.created_at DESC,products.id DESC";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível carregar seus anúncios.");
        return;
    }
    sqlite3_bind_int64(statement, 1, user_id);
    struct json_object *response = json_object_new_object();
    struct json_object *products = json_object_new_array();
    while (sqlite3_step(statement) == SQLITE_ROW) {
        json_object_array_add(products, product_json(statement));
    }
    sqlite3_finalize(statement);
    json_object_object_add(response, "products", products);
    respond_json(client, "200 OK", response, NULL);
}

static void handle_product_review(int client, int64_t product_id, int64_t user_id,
                                  struct json_object *body) {
    int64_t rating = 0;
    if (json_integer(body, "rating", &rating) != 0 || rating < 1 || rating > 5) {
        respond_error(client, "400 Bad Request", "A avaliação deve ter de 1 a 5 estrelas.");
        return;
    }

    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, "SELECT 1 FROM products WHERE id=? AND active=1",
                           -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível verificar o produto.");
        return;
    }
    sqlite3_bind_int64(statement, 1, product_id);
    int result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW) {
        respond_error(client, result == SQLITE_DONE ? "404 Not Found" : "500 Internal Server Error",
                      result == SQLITE_DONE ? "Produto não encontrado." : "Não foi possível verificar o produto.");
        return;
    }

    if (sqlite3_prepare_v2(database,
            "SELECT 1 FROM order_items JOIN orders ON orders.id=order_items.order_id "
            "WHERE order_items.product_id=? AND orders.buyer_id=? "
            "AND orders.status='paid' AND orders.payment_status='approved' LIMIT 1",
            -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível verificar sua compra.");
        return;
    }
    sqlite3_bind_int64(statement, 1, product_id);
    sqlite3_bind_int64(statement, 2, user_id);
    result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW) {
        respond_error(client, result == SQLITE_DONE ? "403 Forbidden" : "500 Internal Server Error",
                      result == SQLITE_DONE
                        ? "A avaliação fica disponível após a confirmação do pagamento deste produto."
                        : "Não foi possível verificar sua compra.");
        return;
    }

    if (sqlite3_prepare_v2(database,
            "INSERT INTO product_reviews(product_id,user_id,rating) VALUES(?,?,?) "
            "ON CONFLICT(product_id,user_id) DO UPDATE SET rating=excluded.rating,created_at=CURRENT_TIMESTAMP",
            -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível salvar sua avaliação.");
        return;
    }
    sqlite3_bind_int64(statement, 1, product_id);
    sqlite3_bind_int64(statement, 2, user_id);
    sqlite3_bind_int64(statement, 3, rating);
    result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        respond_error(client, "500 Internal Server Error", "Não foi possível salvar sua avaliação.");
        return;
    }

    if (sqlite3_prepare_v2(database,
            "SELECT AVG(rating),COUNT(*) FROM product_reviews WHERE product_id=?",
            -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Avaliação salva, mas não foi possível atualizar a média.");
        return;
    }
    sqlite3_bind_int64(statement, 1, product_id);
    result = sqlite3_step(statement);
    double average = result == SQLITE_ROW ? sqlite3_column_double(statement, 0) : 0;
    int64_t count = result == SQLITE_ROW ? sqlite3_column_int64(statement, 1) : 0;
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW) {
        respond_error(client, "500 Internal Server Error", "Avaliação salva, mas não foi possível atualizar a média.");
        return;
    }

    struct json_object *response = json_object_new_object();
    json_object_object_add(response, "ratingAverage", json_object_new_double(average));
    json_object_object_add(response, "ratingCount", json_object_new_int64(count));
    respond_json(client, "200 OK", response, NULL);
}

static void handle_orders(int client, int64_t user_id) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database,
            "SELECT id,status,total_cents,shipping_cents,created_at,payment_status,checkout_url "
            "FROM orders WHERE buyer_id=? ORDER BY id DESC",
            -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível carregar seus pedidos.");
        return;
    }
    sqlite3_bind_int64(statement, 1, user_id);
    struct json_object *response = json_object_new_object();
    struct json_object *orders = json_object_new_array();
    while (sqlite3_step(statement) == SQLITE_ROW) {
        struct json_object *order = json_object_new_object();
        json_object_object_add(order, "id", json_object_new_int64(sqlite3_column_int64(statement, 0)));
        json_object_object_add(order, "status", json_object_new_string((const char *)sqlite3_column_text(statement, 1)));
        json_object_object_add(order, "totalCents", json_object_new_int64(sqlite3_column_int64(statement, 2)));
        json_object_object_add(order, "shippingCents", json_object_new_int64(sqlite3_column_int64(statement, 3)));
        json_object_object_add(order, "createdAt", json_object_new_string((const char *)sqlite3_column_text(statement, 4)));
        json_object_object_add(order, "paymentStatus", json_object_new_string((const char *)sqlite3_column_text(statement, 5)));
        json_object_object_add(order, "checkoutUrl", json_object_new_string((const char *)sqlite3_column_text(statement, 6)));
        sqlite3_stmt *shipments = NULL;
        if (sqlite3_prepare_v2(database,
                "SELECT users.name,order_shipments.status,order_shipments.tracking_code "
                "FROM order_shipments JOIN users ON users.id=order_shipments.seller_id "
                "WHERE order_shipments.order_id=?",
                -1, &shipments, NULL) != SQLITE_OK) {
            json_object_put(order);
            sqlite3_finalize(statement);
            json_object_put(response);
            respond_error(client, "500 Internal Server Error", "Não foi possível carregar o acompanhamento da entrega.");
            return;
        }
        sqlite3_bind_int64(shipments, 1, sqlite3_column_int64(statement, 0));
        struct json_object *shipment_list = json_object_new_array();
        int shipment_result;
        while ((shipment_result = sqlite3_step(shipments)) == SQLITE_ROW) {
            struct json_object *shipment = json_object_new_object();
            json_object_object_add(shipment, "sellerName",
                json_object_new_string((const char *)sqlite3_column_text(shipments, 0)));
            json_object_object_add(shipment, "status",
                json_object_new_string((const char *)sqlite3_column_text(shipments, 1)));
            json_object_object_add(shipment, "trackingCode",
                json_object_new_string((const char *)sqlite3_column_text(shipments, 2)));
            json_object_array_add(shipment_list, shipment);
        }
        sqlite3_finalize(shipments);
        if (shipment_result != SQLITE_DONE) {
            json_object_put(shipment_list);
            json_object_put(order);
            sqlite3_finalize(statement);
            json_object_put(response);
            respond_error(client, "500 Internal Server Error", "Não foi possível carregar o acompanhamento da entrega.");
            return;
        }
        json_object_object_add(order, "shipments", shipment_list);
        json_object_array_add(orders, order);
    }
    sqlite3_finalize(statement);
    json_object_object_add(response, "orders", orders);
    respond_json(client, "200 OK", response, NULL);
}

static void handle_seller_orders(int client, int64_t user_id) {
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT orders.id,orders.status,order_shipments.status,order_shipments.tracking_code,"
        "(SELECT COALESCE(SUM((items.unit_price_cents+items.shipping_cents)*items.quantity),0) "
        "FROM order_items items WHERE items.order_id=orders.id AND items.seller_id=?),"
        "(SELECT COALESCE(SUM(items.shipping_cents*items.quantity),0) FROM order_items items "
        "WHERE items.order_id=orders.id AND items.seller_id=?),orders.created_at,users.name,"
        "orders.recipient_name,orders.recipient_phone,orders.postal_code,orders.state,orders.city,"
        "orders.street,orders.address_number,orders.complement,"
        "COALESCE((SELECT GROUP_CONCAT(order_items.product_name || ' × ' || order_items.quantity, ', ') "
        "FROM order_items WHERE order_items.order_id=orders.id AND order_items.seller_id=?),'') "
        "FROM order_shipments JOIN orders ON orders.id=order_shipments.order_id "
        "JOIN users ON users.id=orders.buyer_id WHERE order_shipments.seller_id=? "
        "ORDER BY orders.id DESC";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível carregar os pedidos para envio.");
        return;
    }
    sqlite3_bind_int64(statement, 1, user_id);
    sqlite3_bind_int64(statement, 2, user_id);
    sqlite3_bind_int64(statement, 3, user_id);
    struct json_object *response = json_object_new_object();
    struct json_object *orders = json_object_new_array();
    int result;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        struct json_object *order = json_object_new_object();
        json_object_object_add(order, "id", json_object_new_int64(sqlite3_column_int64(statement, 0)));
        json_object_object_add(order, "status", json_object_new_string((const char *)sqlite3_column_text(statement, 1)));
        json_object_object_add(order, "shipmentStatus", json_object_new_string((const char *)sqlite3_column_text(statement, 2)));
        json_object_object_add(order, "trackingCode", json_object_new_string((const char *)sqlite3_column_text(statement, 3)));
        json_object_object_add(order, "totalCents", json_object_new_int64(sqlite3_column_int64(statement, 4)));
        json_object_object_add(order, "shippingCents", json_object_new_int64(sqlite3_column_int64(statement, 5)));
        json_object_object_add(order, "createdAt", json_object_new_string((const char *)sqlite3_column_text(statement, 6)));
        json_object_object_add(order, "buyerName", json_object_new_string((const char *)sqlite3_column_text(statement, 7)));
        if (strcmp((const char *)sqlite3_column_text(statement, 1), "paid") == 0 ||
            strcmp((const char *)sqlite3_column_text(statement, 1), "refunded") == 0) {
            json_object_object_add(order, "recipientName", json_object_new_string((const char *)sqlite3_column_text(statement, 8)));
            json_object_object_add(order, "recipientPhone", json_object_new_string((const char *)sqlite3_column_text(statement, 9)));
            json_object_object_add(order, "postalCode", json_object_new_string((const char *)sqlite3_column_text(statement, 10)));
            json_object_object_add(order, "state", json_object_new_string((const char *)sqlite3_column_text(statement, 11)));
            json_object_object_add(order, "city", json_object_new_string((const char *)sqlite3_column_text(statement, 12)));
            json_object_object_add(order, "street", json_object_new_string((const char *)sqlite3_column_text(statement, 13)));
            json_object_object_add(order, "addressNumber", json_object_new_string((const char *)sqlite3_column_text(statement, 14)));
            json_object_object_add(order, "complement", json_object_new_string((const char *)sqlite3_column_text(statement, 15)));
        }
        json_object_object_add(order, "products", json_object_new_string((const char *)sqlite3_column_text(statement, 16)));
        json_object_array_add(orders, order);
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        json_object_put(response);
        respond_error(client, "500 Internal Server Error", "Não foi possível carregar os pedidos para envio.");
        return;
    }
    json_object_object_add(response, "orders", orders);
    respond_json(client, "200 OK", response, NULL);
}

static void handle_seller_order_update(int client, struct json_object *body,
                                       int64_t user_id, int64_t order_id) {
    const char *status = json_string(body, "status");
    const char *tracking_code = json_string(body, "trackingCode");
    if (status == NULL || (strcmp(status, "shipped") != 0 && strcmp(status, "delivered") != 0) ||
        (tracking_code != NULL && !valid_address_field(tracking_code, 0, 80))) {
        respond_error(client, "400 Bad Request", "Status de entrega ou código de rastreio inválido.");
        return;
    }
    const char *previous_status = strcmp(status, "shipped") == 0 ? "awaiting_shipment" : "shipped";
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "UPDATE order_shipments SET status=?,"
        "tracking_code=CASE WHEN ?='' THEN tracking_code ELSE ? END,updated_at=CURRENT_TIMESTAMP "
        "WHERE order_id=? AND seller_id=? AND status=? "
        "AND EXISTS(SELECT 1 FROM orders WHERE orders.id=order_shipments.order_id AND orders.status='paid')";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível atualizar a entrega.");
        return;
    }
    sqlite3_bind_text(statement, 1, status, -1, SQLITE_TRANSIENT);
    const char *tracking_value = tracking_code == NULL ? "" : tracking_code;
    sqlite3_bind_text(statement, 2, tracking_value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, tracking_value, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 4, order_id);
    sqlite3_bind_int64(statement, 5, user_id);
    sqlite3_bind_text(statement, 6, previous_status, -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(statement);
    int changed = sqlite3_changes(database);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        respond_error(client, "500 Internal Server Error", "Não foi possível atualizar a entrega.");
    } else if (changed == 0) {
        respond_error(client, "409 Conflict",
            "O pedido não está pago, não pertence à sua loja ou já avançou para outra etapa.");
    } else {
        struct json_object *response = json_object_new_object();
        json_object_object_add(response, "ok", json_object_new_boolean(1));
        respond_json(client, "200 OK", response, NULL);
    }
}

static void handle_create_product(int client, struct json_object *body, int64_t user_id) {
    sqlite3_stmt *profile_statement = NULL;
    if (sqlite3_prepare_v2(database, "SELECT 1 FROM seller_profiles WHERE user_id=?",
                           -1, &profile_statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível verificar o cadastro de vendedor.");
        return;
    }
    sqlite3_bind_int64(profile_statement, 1, user_id);
    int profile_result = sqlite3_step(profile_statement);
    sqlite3_finalize(profile_statement);
    if (profile_result == SQLITE_DONE) {
        respond_error(client, "403 Forbidden", "Conclua o cadastro de vendedor antes de publicar produtos.");
        return;
    }
    if (profile_result != SQLITE_ROW) {
        respond_error(client, "500 Internal Server Error", "Não foi possível verificar o cadastro de vendedor.");
        return;
    }
    const char *name = json_string(body, "name");
    const char *description = json_string(body, "description");
    const char *category = json_string(body, "category");
    const char *image_url = json_string(body, "imageUrl");
    int64_t price_cents = 0, shipping_cents = 0;
    if (name == NULL || description == NULL || category == NULL || image_url == NULL ||
        json_integer(body, "priceCents", &price_cents) != 0 ||
        json_integer(body, "shippingCents", &shipping_cents) != 0 ||
        strlen(name) < 2 || strlen(name) > 120 || strlen(description) > 2000 ||
        strlen(description) < 2 || strlen(category) < 2 || strlen(category) > 60 ||
        price_cents < 1 || price_cents > 100000000000LL ||
        shipping_cents < 0 || shipping_cents > 100000000000LL ||
        image_url[0] == '\0' || strlen(image_url) > 1000 || strncmp(image_url, "https://", 8) != 0) {
        respond_error(client, "400 Bad Request",
                      "Confira nome, categoria, preços, frete por unidade e URL de imagem HTTPS.");
        return;
    }
    sqlite3_stmt *statement = NULL;
    const char *sql = "INSERT INTO products(seller_id,name,description,category,price_cents,shipping_cents,image_url) VALUES(?,?,?,?,?,?,?)";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível publicar o produto.");
        return;
    }
    sqlite3_bind_int64(statement, 1, user_id);
    sqlite3_bind_text(statement, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, description, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, category, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 5, price_cents);
    sqlite3_bind_int64(statement, 6, shipping_cents);
    sqlite3_bind_text(statement, 7, image_url == NULL ? "" : image_url, -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        respond_error(client, "500 Internal Server Error", "Não foi possível publicar o produto.");
        return;
    }
    struct json_object *response = json_object_new_object();
    json_object_object_add(response, "id", json_object_new_int64(sqlite3_last_insert_rowid(database)));
    json_object_object_add(response, "message", json_object_new_string("Produto publicado."));
    respond_json(client, "201 Created", response, NULL);
}

static struct json_object *cart_json(int64_t user_id) {
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "SELECT products.id,products.name,products.price_cents,products.image_url,"
        "cart_items.quantity,products.seller_id,products.shipping_cents,users.name "
        "FROM cart_items JOIN products ON products.id=cart_items.product_id "
        "JOIN users ON users.id=products.seller_id "
        "WHERE cart_items.user_id=? AND products.active=1 ORDER BY products.name";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) return NULL;
    sqlite3_bind_int64(statement, 1, user_id);
    struct json_object *response = json_object_new_object();
    struct json_object *items = json_object_new_array();
    int64_t total = 0;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        struct json_object *item = json_object_new_object();
        int64_t price = sqlite3_column_int64(statement, 2);
        int quantity = sqlite3_column_int(statement, 4);
        int64_t shipping = sqlite3_column_int64(statement, 6);
        json_object_object_add(item, "id", json_object_new_int64(sqlite3_column_int64(statement, 0)));
        json_object_object_add(item, "name", json_object_new_string((const char *)sqlite3_column_text(statement, 1)));
        json_object_object_add(item, "priceCents", json_object_new_int64(price));
        json_object_object_add(item, "imageUrl", json_object_new_string((const char *)sqlite3_column_text(statement, 3)));
        json_object_object_add(item, "quantity", json_object_new_int(quantity));
        json_object_object_add(item, "sellerId", json_object_new_int64(sqlite3_column_int64(statement, 5)));
        json_object_object_add(item, "shippingCents", json_object_new_int64(shipping));
        json_object_object_add(item, "sellerName", json_object_new_string((const char *)sqlite3_column_text(statement, 7)));
        total += price * quantity;
        total += shipping * quantity;
        json_object_array_add(items, item);
    }
    sqlite3_finalize(statement);
    json_object_object_add(response, "items", items);
    json_object_object_add(response, "totalCents", json_object_new_int64(total));
    return response;
}

static void handle_add_cart(int client, struct json_object *body, int64_t user_id) {
    int64_t product_id = 0, quantity = 0;
    if (json_integer(body, "productId", &product_id) != 0 ||
        json_integer(body, "quantity", &quantity) != 0 ||
        product_id < 1 || quantity < 1 || quantity > 20) {
        respond_error(client, "400 Bad Request", "Produto ou quantidade inválidos.");
        return;
    }
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database,
            "INSERT INTO cart_items(user_id,product_id,quantity) "
            "SELECT ?,id,? FROM products WHERE id=? AND active=1 "
            "ON CONFLICT(user_id,product_id) DO UPDATE SET quantity=cart_items.quantity+excluded.quantity "
            "WHERE cart_items.quantity+excluded.quantity<=20",
            -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível atualizar a sacola.");
        return;
    }
    sqlite3_bind_int64(statement, 1, user_id);
    sqlite3_bind_int64(statement, 2, quantity);
    sqlite3_bind_int64(statement, 3, product_id);
    int result = sqlite3_step(statement);
    int changed = sqlite3_changes(database);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        respond_error(client, "500 Internal Server Error", "Não foi possível atualizar a sacola.");
    } else if (changed == 0) {
        respond_error(client, "400 Bad Request", "Produto indisponível ou limite de quantidade excedido.");
    } else {
        struct json_object *response = cart_json(user_id);
        if (response == NULL) respond_error(client, "500 Internal Server Error", "Não foi possível carregar a sacola.");
        else respond_json(client, "200 OK", response, NULL);
    }
}

static void handle_delete_cart(int client, long product_id, int64_t user_id) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, "DELETE FROM cart_items WHERE user_id=? AND product_id=?",
                           -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível atualizar a sacola.");
        return;
    }
    sqlite3_bind_int64(statement, 1, user_id);
    sqlite3_bind_int64(statement, 2, product_id);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    struct json_object *response = cart_json(user_id);
    if (response == NULL) respond_error(client, "500 Internal Server Error", "Não foi possível carregar a sacola.");
    else respond_json(client, "200 OK", response, NULL);
}

static int valid_address_field(const char *value, size_t minimum, size_t maximum) {
    if (value == NULL) return 0;
    size_t length = strlen(value);
    if (length < minimum || length > maximum) return 0;
    int has_nonspace = 0;
    for (size_t i = 0; i < length; i++) {
        unsigned char character = (unsigned char)value[i];
        if (character < 32 || character == 127) return 0;
        if (!isspace(character)) has_nonspace = 1;
    }
    return minimum == 0 || has_nonspace;
}

static int valid_digits(const char *value, size_t minimum, size_t maximum) {
    size_t digits = 0;
    if (value == NULL) return 0;
    for (size_t i = 0; value[i] != '\0'; i++) {
        if (isdigit((unsigned char)value[i])) digits++;
        else if (value[i] != ' ' && value[i] != '(' && value[i] != ')' &&
                 value[i] != '-' && value[i] != '+') return 0;
    }
    return digits >= minimum && digits <= maximum;
}

static int request_header(const char *request, const char *name, char *value, size_t capacity) {
    const char *line = strstr(request, "\r\n");
    if (line == NULL) return -1;
    line += 2;
    size_t name_length = strlen(name);
    while (*line != '\0' && strncmp(line, "\r\n", 2) != 0) {
        const char *end = strstr(line, "\r\n");
        if (end == NULL) return -1;
        if ((size_t)(end - line) > name_length &&
            strncasecmp(line, name, name_length) == 0 && line[name_length] == ':') {
            const char *start = line + name_length + 1;
            while (start < end && isspace((unsigned char)*start)) start++;
            size_t length = (size_t)(end - start);
            if (length >= capacity) return -1;
            memcpy(value, start, length);
            value[length] = '\0';
            return 0;
        }
        line = end + 2;
    }
    return -1;
}

static int valid_mp_signature(const char *request, const char *payment_id) {
    const char *secret = getenv("MP_WEBHOOK_SECRET");
    char signature[256], request_id[128], manifest[512], provided_hash[65], expected_hash[65];
    if (secret == NULL || secret[0] == '\0' ||
        request_header(request, "x-signature", signature, sizeof(signature)) != 0 ||
        request_header(request, "x-request-id", request_id, sizeof(request_id)) != 0) return 0;
    char signature_copy[sizeof(signature)];
    snprintf(signature_copy, sizeof(signature_copy), "%s", signature);
    char *timestamp = NULL, *hash = NULL;
    char *token = strtok(signature_copy, ",");
    while (token != NULL) {
        while (isspace((unsigned char)*token)) token++;
        if (strncmp(token, "ts=", 3) == 0) timestamp = token + 3;
        else if (strncmp(token, "v1=", 3) == 0) hash = token + 3;
        token = strtok(NULL, ",");
    }
    if (timestamp == NULL || hash == NULL || strlen(hash) != 64) return 0;
    char *timestamp_end = NULL;
    errno = 0;
    long long timestamp_value = strtoll(timestamp, &timestamp_end, 10);
    if (errno != 0 || timestamp_end == timestamp || *timestamp_end != '\0') return 0;
    if (timestamp_value < 0) return 0;
    for (size_t i = 0; i < 64; i++) {
        if (!isxdigit((unsigned char)hash[i])) return 0;
        provided_hash[i] = (char)tolower((unsigned char)hash[i]);
    }
    provided_hash[64] = '\0';
    int length = snprintf(manifest, sizeof(manifest), "id:%s;request-id:%s;ts:%s;",
                          payment_id, request_id, timestamp);
    if (length < 0 || (size_t)length >= sizeof(manifest)) return 0;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    if (HMAC(EVP_sha256(), secret, (int)strlen(secret), (unsigned char *)manifest,
             (size_t)length, digest, &digest_length) == NULL || digest_length != 32) return 0;
    bytes_to_hex(digest, digest_length, expected_hash);
    OPENSSL_cleanse(digest, sizeof(digest));
    return CRYPTO_memcmp(expected_hash, provided_hash, sizeof(provided_hash)) == 0;
}

static void handle_mercadopago_webhook(int client, const char *request, const char *path,
                                       const char *body, size_t body_length) {
    if (!mercadopago_configured()) {
        respond_error(client, "503 Service Unavailable", "As notificações de pagamento não estão configuradas.");
        return;
    }
    const char *query = strchr(path, '?');
    const char *query_id = query == NULL ? NULL : strstr(query + 1, "data.id=");
    char payment_id[64] = {0};
    if (query_id != NULL) {
        query_id += strlen("data.id=");
        size_t length = 0;
        while (isdigit((unsigned char)query_id[length]) && length + 1 < sizeof(payment_id)) length++;
        if (length > 0 && (query_id[length] == '\0' || query_id[length] == '&')) {
            memcpy(payment_id, query_id, length);
            payment_id[length] = '\0';
        }
    }
    struct json_object *notification = body_length > 0 ? parse_json(body, body_length) : NULL;
    if (notification != NULL) {
        struct json_object *data = NULL;
        if (json_object_object_get_ex(notification, "data", &data)) {
            const char *body_id = json_string(data, "id");
            if (body_id != NULL && payment_id[0] == '\0' && strlen(body_id) < sizeof(payment_id))
                snprintf(payment_id, sizeof(payment_id), "%s", body_id);
        }
    }
    if (notification != NULL) json_object_put(notification);
    if (payment_id[0] == '\0' || !valid_mp_signature(request, payment_id)) {
        respond_error(client, "401 Unauthorized", "Assinatura da notificação inválida.");
        return;
    }
    for (size_t i = 0; payment_id[i] != '\0'; i++) {
        if (!isdigit((unsigned char)payment_id[i])) {
            respond_error(client, "400 Bad Request", "Identificador de pagamento inválido.");
            return;
        }
    }
    char payment_url[128], authorization[4096];
    snprintf(payment_url, sizeof(payment_url), "https://api.mercadopago.com/v1/payments/%s", payment_id);
    int auth_length = snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s",
                               getenv("MP_ACCESS_TOKEN"));
    if (auth_length < 0 || (size_t)auth_length >= sizeof(authorization)) {
        respond_error(client, "500 Internal Server Error", "Não foi possível consultar o pagamento.");
        return;
    }
    struct json_object *payment = NULL;
    if (http_get_json(payment_url, authorization, &payment) != 0) {
        respond_error(client, "502 Bad Gateway", "Não foi possível confirmar o pagamento no Mercado Pago.");
        return;
    }
    OPENSSL_cleanse(authorization, sizeof(authorization));
    const char *external_reference = json_string(payment, "external_reference");
    const char *status = json_string(payment, "status");
    const char *currency = json_string(payment, "currency_id");
    struct json_object *amount_value = NULL;
    double amount = -1;
    if (json_object_object_get_ex(payment, "transaction_amount", &amount_value))
        amount = json_object_get_double(amount_value);
    int valid_payment = external_reference != NULL && strlen(external_reference) == 32 &&
        status != NULL && currency != NULL && strcmp(currency, "BRL") == 0 &&
        amount >= 0 && amount < 10000000000.0;
    if (!valid_payment) {
        json_object_put(payment);
        respond_error(client, "400 Bad Request", "Os dados do pagamento não correspondem a um pedido válido.");
        return;
    }
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database,
            "SELECT id,total_cents,status FROM orders WHERE external_reference=?",
            -1, &statement, NULL) != SQLITE_OK) {
        json_object_put(payment);
        respond_error(client, "500 Internal Server Error", "Não foi possível localizar o pedido do pagamento.");
        return;
    }
    sqlite3_bind_text(statement, 1, external_reference, -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(statement);
    if (result == SQLITE_DONE) {
        sqlite3_finalize(statement);
        json_object_put(payment);
        struct json_object *response = json_object_new_object();
        json_object_object_add(response, "ok", json_object_new_boolean(1));
        respond_json(client, "200 OK", response, NULL);
        return;
    }
    if (result != SQLITE_ROW) {
        sqlite3_finalize(statement);
        json_object_put(payment);
        respond_error(client, "500 Internal Server Error", "Não foi possível localizar o pedido do pagamento.");
        return;
    }
    int64_t order_id = sqlite3_column_int64(statement, 0);
    int64_t total_cents = sqlite3_column_int64(statement, 1);
    const char *order_status_raw = (const char *)sqlite3_column_text(statement, 2);
    char order_status[32];
    snprintf(order_status, sizeof(order_status), "%s", order_status_raw == NULL ? "" : order_status_raw);
    sqlite3_finalize(statement);
    int64_t amount_cents = (int64_t)(amount * 100.0 + 0.5);
    int is_refund = strcmp(status, "refunded") == 0 || strcmp(status, "charged_back") == 0;
    if (amount_cents != total_cents ||
        (strcmp(order_status, "awaiting_payment") != 0 &&
         !(is_refund && (strcmp(order_status, "paid") == 0 ||
                         strcmp(order_status, "refunded") == 0)))) {
        if (amount_cents == total_cents) {
            json_object_put(payment);
            struct json_object *response = json_object_new_object();
            json_object_object_add(response, "ok", json_object_new_boolean(1));
            respond_json(client, "200 OK", response, NULL);
            return;
        }
        json_object_put(payment);
        respond_error(client, "400 Bad Request", "O valor do pagamento não corresponde ao total do pedido.");
        return;
    }
    if (strcmp(status, "approved") == 0) {
        if (execute_sql("BEGIN IMMEDIATE") != 0) {
            json_object_put(payment);
            respond_error(client, "500 Internal Server Error", "Não foi possível confirmar o pagamento.");
            return;
        }
        if (sqlite3_prepare_v2(database,
                "UPDATE orders SET status='paid',payment_status=?,payment_id=? "
                "WHERE id=? AND status='awaiting_payment'",
                -1, &statement, NULL) != SQLITE_OK) {
            execute_sql("ROLLBACK");
            json_object_put(payment);
            respond_error(client, "500 Internal Server Error", "Não foi possível atualizar o status do pedido.");
            return;
        }
        sqlite3_bind_text(statement, 1, status, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, payment_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 3, order_id);
        result = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (result == SQLITE_DONE && sqlite3_prepare_v2(database,
                "UPDATE order_shipments SET status='awaiting_shipment',updated_at=CURRENT_TIMESTAMP "
                "WHERE order_id=? AND status='awaiting_payment'",
                -1, &statement, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, order_id);
            result = sqlite3_step(statement);
            sqlite3_finalize(statement);
        } else if (result == SQLITE_DONE) {
            result = SQLITE_ERROR;
        }
        if (result != SQLITE_DONE || execute_sql("COMMIT") != 0) {
            execute_sql("ROLLBACK");
            json_object_put(payment);
            respond_error(client, "500 Internal Server Error", "O pagamento foi confirmado, mas não foi possível atualizar a entrega.");
            return;
        }
    } else {
        if (sqlite3_prepare_v2(database,
                "UPDATE orders SET payment_status=?,payment_id=?,"
                "status=CASE WHEN ? THEN 'refunded' ELSE status END "
                "WHERE id=? AND (status='awaiting_payment' OR (? AND status IN ('paid','refunded')))",
                -1, &statement, NULL) != SQLITE_OK) {
            json_object_put(payment);
            respond_error(client, "500 Internal Server Error", "Não foi possível atualizar o status do pagamento.");
            return;
        }
        sqlite3_bind_text(statement, 1, status, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, payment_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 3, is_refund);
        sqlite3_bind_int64(statement, 4, order_id);
        sqlite3_bind_int(statement, 5, is_refund);
        result = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (result != SQLITE_DONE) {
            json_object_put(payment);
            respond_error(client, "500 Internal Server Error", "Não foi possível atualizar o status do pagamento.");
            return;
        }
    }
    json_object_put(payment);
    struct json_object *response = json_object_new_object();
    json_object_object_add(response, "ok", json_object_new_boolean(1));
    respond_json(client, "200 OK", response, NULL);
}

static void handle_order(int client, struct json_object *body, int64_t user_id) {
    if (!mercadopago_configured()) {
        respond_error(client, "503 Service Unavailable",
            "O pagamento ainda não está configurado. O administrador deve configurar Mercado Pago e a URL HTTPS de notificações.");
        return;
    }
    const char *recipient_name = json_string(body, "recipientName");
    const char *recipient_phone = json_string(body, "recipientPhone");
    const char *postal_code = json_string(body, "postalCode");
    const char *state = json_string(body, "state");
    const char *city = json_string(body, "city");
    const char *street = json_string(body, "street");
    const char *address_number = json_string(body, "addressNumber");
    const char *complement = json_string(body, "complement");
    if (!valid_address_field(recipient_name, 2, 100) ||
        !valid_digits(recipient_phone, 10, 11) ||
        !valid_digits(postal_code, 8, 8) ||
        !valid_address_field(state, 2, 80) || !valid_address_field(city, 2, 100) ||
        !valid_address_field(street, 2, 160) ||
        !valid_address_field(address_number, 1, 20) ||
        (complement != NULL && !valid_address_field(complement, 0, 100))) {
        respond_error(client, "400 Bad Request", "Confira nome, telefone, CEP e endereço de entrega.");
        return;
    }
    if (complement == NULL) complement = "";
    if (execute_sql("BEGIN IMMEDIATE") != 0) {
        respond_error(client, "500 Internal Server Error", "Não foi possível iniciar o pedido.");
        return;
    }
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database,
            "SELECT products.id,products.seller_id,products.name,products.price_cents,"
            "products.shipping_cents,cart_items.quantity "
            "FROM cart_items JOIN products ON products.id=cart_items.product_id "
            "WHERE cart_items.user_id=? AND products.active=1",
            -1, &statement, NULL) != SQLITE_OK) {
        execute_sql("ROLLBACK");
        respond_error(client, "500 Internal Server Error", "Não foi possível verificar a sacola.");
        return;
    }
    sqlite3_bind_int64(statement, 1, user_id);
    struct json_object *items = json_object_new_array();
    struct json_object *preference_items = json_object_new_array();
    int64_t total = 0, shipping_total = 0;
    int item_count = 0, result;
    int total_exceeded = 0;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        int64_t price = sqlite3_column_int64(statement, 3);
        int64_t shipping = sqlite3_column_int64(statement, 4);
        int quantity = sqlite3_column_int(statement, 5);
        int64_t item_total = price * quantity;
        int64_t item_shipping = shipping * quantity;
        if (item_total < 0 || item_shipping < 0 ||
            total > 100000000000LL - item_total ||
            shipping_total > 100000000000LL - item_shipping ||
            total + item_total > 100000000000LL - shipping_total - item_shipping) {
            total_exceeded = 1;
            break;
        }
        struct json_object *item = json_object_new_object();
        json_object_object_add(item, "id", json_object_new_int64(sqlite3_column_int64(statement, 0)));
        json_object_object_add(item, "sellerId", json_object_new_int64(sqlite3_column_int64(statement, 1)));
        json_object_object_add(item, "name", json_object_new_string((const char *)sqlite3_column_text(statement, 2)));
        json_object_object_add(item, "priceCents", json_object_new_int64(price));
        json_object_object_add(item, "shippingCents", json_object_new_int64(shipping));
        json_object_object_add(item, "quantity", json_object_new_int(quantity));
        json_object_array_add(items, item);
        struct json_object *payment_item = json_object_new_object();
        json_object_object_add(payment_item, "title",
            json_object_new_string((const char *)sqlite3_column_text(statement, 2)));
        json_object_object_add(payment_item, "quantity", json_object_new_int(quantity));
        json_object_object_add(payment_item, "unit_price",
            json_object_new_double((double)price / 100.0));
        json_object_object_add(payment_item, "currency_id", json_object_new_string("BRL"));
        json_object_array_add(preference_items, payment_item);
        total += price * quantity;
        shipping_total += shipping * quantity;
        item_count++;
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        json_object_put(items);
        json_object_put(preference_items);
        execute_sql("ROLLBACK");
        respond_error(client, "500 Internal Server Error", "Não foi possível verificar os produtos da sacola.");
        return;
    }
    if (total_exceeded) {
        json_object_put(items);
        json_object_put(preference_items);
        execute_sql("ROLLBACK");
        respond_error(client, "400 Bad Request", "O total da compra ultrapassa o limite permitido.");
        return;
    }
    if (item_count == 0) {
        json_object_put(items);
        json_object_put(preference_items);
        execute_sql("ROLLBACK");
        respond_error(client, "400 Bad Request", "Adicione um produto publicado à sacola antes de continuar.");
        return;
    }
    total += shipping_total;
    if (shipping_total > 0) {
        struct json_object *freight = json_object_new_object();
        json_object_object_add(freight, "title",
            json_object_new_string("Frete informado pelo vendedor"));
        json_object_object_add(freight, "quantity", json_object_new_int(1));
        json_object_object_add(freight, "unit_price",
            json_object_new_double((double)shipping_total / 100.0));
        json_object_object_add(freight, "currency_id", json_object_new_string("BRL"));
        json_object_array_add(preference_items, freight);
    }
    unsigned char reference_bytes[16];
    char external_reference[33];
    if (RAND_bytes(reference_bytes, sizeof(reference_bytes)) != 1) {
        json_object_put(items);
        json_object_put(preference_items);
        execute_sql("ROLLBACK");
        respond_error(client, "500 Internal Server Error", "Não foi possível preparar o pagamento.");
        return;
    }
    bytes_to_hex(reference_bytes, sizeof(reference_bytes), external_reference);
    OPENSSL_cleanse(reference_bytes, sizeof(reference_bytes));
    const char *base_url = getenv("PUBLIC_BASE_URL");
    char notification_url[640], success_url[640], pending_url[640], failure_url[640];
    snprintf(notification_url, sizeof(notification_url), "%s/api/payments/mercadopago/webhook", base_url);
    snprintf(success_url, sizeof(success_url), "%s/?payment=success", base_url);
    snprintf(pending_url, sizeof(pending_url), "%s/?payment=pending", base_url);
    snprintf(failure_url, sizeof(failure_url), "%s/?payment=failure", base_url);
    struct json_object *preference = json_object_new_object();
    json_object_object_add(preference, "items", preference_items);
    json_object_object_add(preference, "external_reference",
                           json_object_new_string(external_reference));
    json_object_object_add(preference, "notification_url",
                           json_object_new_string(notification_url));
    struct json_object *back_urls = json_object_new_object();
    json_object_object_add(back_urls, "success", json_object_new_string(success_url));
    json_object_object_add(back_urls, "pending", json_object_new_string(pending_url));
    json_object_object_add(back_urls, "failure", json_object_new_string(failure_url));
    json_object_object_add(preference, "back_urls", back_urls);
    json_object_object_add(preference, "auto_return", json_object_new_string("approved"));
    struct json_object *preference_response = NULL;
    if (http_post_json("https://api.mercadopago.com/checkout/preferences",
                       getenv("MP_ACCESS_TOKEN"), preference, &preference_response) != 0) {
        json_object_put(preference);
        json_object_put(items);
        execute_sql("ROLLBACK");
        respond_error(client, "502 Bad Gateway", "O Mercado Pago não criou a cobrança. Confira a configuração e tente novamente.");
        return;
    }
    json_object_put(preference);
    const char *preference_id = json_string(preference_response, "id");
    const char *checkout_url = json_string(preference_response, "init_point");
    if (preference_id == NULL || strlen(preference_id) > 200 ||
        checkout_url == NULL || strncmp(checkout_url, "https://www.mercadopago.com", 27) != 0 ||
        strlen(checkout_url) > 2000) {
        json_object_put(preference_response);
        json_object_put(items);
        execute_sql("ROLLBACK");
        respond_error(client, "502 Bad Gateway", "O Mercado Pago retornou uma resposta de cobrança inválida.");
        return;
    }
    char checkout_url_copy[2001];
    snprintf(checkout_url_copy, sizeof(checkout_url_copy), "%s", checkout_url);
    char preference_id_copy[201];
    snprintf(preference_id_copy, sizeof(preference_id_copy), "%s", preference_id);
    json_object_put(preference_response);
    if (sqlite3_prepare_v2(database,
            "INSERT INTO orders(buyer_id,total_cents,shipping_cents,recipient_name,recipient_phone,"
            "postal_code,state,city,street,address_number,complement,external_reference,preference_id,checkout_url) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                           -1, &statement, NULL) != SQLITE_OK) {
        json_object_put(items);
        execute_sql("ROLLBACK");
        respond_error(client, "500 Internal Server Error", "Não foi possível registrar o pedido.");
        return;
    }
    sqlite3_bind_int64(statement, 1, user_id);
    sqlite3_bind_int64(statement, 2, total);
    sqlite3_bind_int64(statement, 3, shipping_total);
    sqlite3_bind_text(statement, 4, recipient_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 5, recipient_phone, -1, SQLITE_TRANSIENT);
    char normalized_postal_code[9];
    size_t postal_index = 0;
    for (size_t i = 0; postal_code[i] != '\0'; i++) {
        if (isdigit((unsigned char)postal_code[i]))
            normalized_postal_code[postal_index++] = postal_code[i];
    }
    normalized_postal_code[postal_index] = '\0';
    sqlite3_bind_text(statement, 6, normalized_postal_code, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 7, state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 8, city, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 9, street, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 10, address_number, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 11, complement, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 12, external_reference, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 13, preference_id_copy, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 14, checkout_url_copy, -1, SQLITE_TRANSIENT);
    result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        json_object_put(items);
        execute_sql("ROLLBACK");
        respond_error(client, "500 Internal Server Error", "Não foi possível registrar o pedido.");
        return;
    }
    int64_t order_id = sqlite3_last_insert_rowid(database);
    for (size_t i = 0; i < json_object_array_length(items); i++) {
        struct json_object *item = json_object_array_get_idx(items, i);
        int64_t product_id = json_object_get_int64(json_object_object_get(item, "id"));
        int64_t seller_id = json_object_get_int64(json_object_object_get(item, "sellerId"));
        int64_t price = json_object_get_int64(json_object_object_get(item, "priceCents"));
        int64_t shipping = json_object_get_int64(json_object_object_get(item, "shippingCents"));
        int quantity = json_object_get_int(json_object_object_get(item, "quantity"));
        const char *name = json_object_get_string(json_object_object_get(item, "name"));
        if (sqlite3_prepare_v2(database,
                "INSERT INTO order_items(order_id,product_id,seller_id,product_name,unit_price_cents,"
                "shipping_cents,quantity) VALUES(?,?,?,?,?,?,?)", -1, &statement, NULL) != SQLITE_OK) {
            json_object_put(items);
            execute_sql("ROLLBACK");
            respond_error(client, "500 Internal Server Error", "Não foi possível registrar os itens do pedido.");
            return;
        }
        sqlite3_bind_int64(statement, 1, order_id);
        sqlite3_bind_int64(statement, 2, product_id);
        sqlite3_bind_int64(statement, 3, seller_id);
        sqlite3_bind_text(statement, 4, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 5, price);
        sqlite3_bind_int64(statement, 6, shipping);
        sqlite3_bind_int(statement, 7, quantity);
        result = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (result != SQLITE_DONE) {
            json_object_put(items);
            execute_sql("ROLLBACK");
            respond_error(client, "500 Internal Server Error", "Não foi possível registrar os itens do pedido.");
            return;
        }
    }
    if (sqlite3_prepare_v2(database,
            "INSERT INTO order_shipments(order_id,seller_id,status) "
            "SELECT order_id,seller_id,'awaiting_payment' FROM order_items "
            "WHERE order_id=? GROUP BY seller_id",
            -1, &statement, NULL) != SQLITE_OK) {
        json_object_put(items);
        execute_sql("ROLLBACK");
        respond_error(client, "500 Internal Server Error", "Não foi possível preparar o acompanhamento da entrega.");
        return;
    }
    sqlite3_bind_int64(statement, 1, order_id);
    result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        json_object_put(items);
        execute_sql("ROLLBACK");
        respond_error(client, "500 Internal Server Error", "Não foi possível preparar o acompanhamento da entrega.");
        return;
    }
    json_object_put(items);
    sqlite3_stmt *clear_cart = NULL;
    if (sqlite3_prepare_v2(database, "DELETE FROM cart_items WHERE user_id=?",
                           -1, &clear_cart, NULL) != SQLITE_OK) {
        execute_sql("ROLLBACK");
        respond_error(client, "500 Internal Server Error", "Não foi possível limpar a sacola.");
        return;
    }
    sqlite3_bind_int64(clear_cart, 1, user_id);
    int clear_result = sqlite3_step(clear_cart);
    sqlite3_finalize(clear_cart);
    if (clear_result != SQLITE_DONE || execute_sql("COMMIT") != 0) {
        execute_sql("ROLLBACK");
        respond_error(client, "500 Internal Server Error", "Não foi possível concluir o registro do pedido.");
        return;
    }
    struct json_object *response = json_object_new_object();
    json_object_object_add(response, "orderId", json_object_new_int64(order_id));
    json_object_object_add(response, "status", json_object_new_string("awaiting_payment"));
    json_object_object_add(response, "totalCents", json_object_new_int64(total));
    json_object_object_add(response, "shippingCents", json_object_new_int64(shipping_total));
    json_object_object_add(response, "checkoutUrl", json_object_new_string(checkout_url_copy));
    json_object_object_add(response, "message", json_object_new_string("Pedido criado. Conclua o pagamento no Mercado Pago."));
    respond_json(client, "201 Created", response, NULL);
}

static void handle_me(int client, int64_t user_id, const char *name, const char *email) {
    struct json_object *response = json_object_new_object();
    struct json_object *user = user_id == 0 ? NULL : user_json(user_id, name, email);
    if (user_id != 0) {
        sqlite3_stmt *statement = NULL;
        if (sqlite3_prepare_v2(database,
                "SELECT birth_date,phone,interests FROM seller_profiles WHERE user_id=?",
                -1, &statement, NULL) != SQLITE_OK) {
            json_object_put(response);
            if (user != NULL) json_object_put(user);
            respond_error(client, "500 Internal Server Error", "Não foi possível carregar o perfil do vendedor.");
            return;
        }
        sqlite3_bind_int64(statement, 1, user_id);
        int result = sqlite3_step(statement);
        if (result == SQLITE_ROW) {
            struct json_object *profile = json_object_new_object();
            json_object_object_add(profile, "birthDate",
                json_object_new_string((const char *)sqlite3_column_text(statement, 0)));
            json_object_object_add(profile, "phone",
                json_object_new_string((const char *)sqlite3_column_text(statement, 1)));
            json_object_object_add(profile, "interests",
                json_object_new_string((const char *)sqlite3_column_text(statement, 2)));
            json_object_object_add(user, "sellerProfile", profile);
        } else if (result == SQLITE_DONE) {
            json_object_object_add(user, "sellerProfile", NULL);
        } else {
            sqlite3_finalize(statement);
            json_object_put(response);
            if (user != NULL) json_object_put(user);
            respond_error(client, "500 Internal Server Error", "Não foi possível carregar o perfil do vendedor.");
            return;
        }
        sqlite3_finalize(statement);
    }
    json_object_object_add(response, "user", user);
    respond_json(client, "200 OK", response, NULL);
}

static int valid_cpf(const char *value) {
    char digits[12];
    size_t count = 0;
    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (isdigit((unsigned char)*cursor)) {
            if (count >= 11) return 0;
            digits[count++] = *cursor;
        } else if (*cursor != '.' && *cursor != '-' && !isspace((unsigned char)*cursor)) {
            return 0;
        }
    }
    if (count != 11) return 0;
    digits[11] = '\0';
    int repeated = 1;
    for (size_t index = 1; index < 11; index++) {
        if (digits[index] != digits[0]) repeated = 0;
    }
    if (repeated) return 0;
    for (int digit = 9; digit < 11; digit++) {
        int sum = 0;
        for (int index = 0; index < digit; index++) {
            sum += (digits[index] - '0') * (digit + 1 - index);
        }
        int check = (sum * 10) % 11;
        if (check == 10) check = 0;
        if (check != digits[digit] - '0') return 0;
    }
    return 1;
}

static int valid_birth_date(const char *value) {
    if (strlen(value) != 10 || value[4] != '-' || value[7] != '-') return 0;
    for (int index = 0; index < 10; index++) {
        if (index != 4 && index != 7 && !isdigit((unsigned char)value[index])) return 0;
    }
    int year = 0, month = 0, day = 0;
    if (sscanf(value, "%4d-%2d-%2d", &year, &month, &day) != 3) return 0;
    if (year < 1900 || month < 1 || month > 12) return 0;
    static const int month_days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int maximum_day = month_days[month - 1];
    if (month == 2 && (year % 400 == 0 || (year % 4 == 0 && year % 100 != 0))) maximum_day++;
    if (day < 1 || day > maximum_day) return 0;
    time_t now = time(NULL);
    struct tm today;
    if (localtime_r(&now, &today) == NULL) return 0;
    int current_year = today.tm_year + 1900;
    int current_month = today.tm_mon + 1;
    int current_day = today.tm_mday;
    return year < current_year ||
        (year == current_year && (month < current_month ||
        (month == current_month && day <= current_day)));
}

static int valid_phone(const char *value) {
    size_t digits = 0;
    if (strlen(value) > 24) return 0;
    for (const char *cursor = value; *cursor != '\0'; cursor++) {
        if (isdigit((unsigned char)*cursor)) digits++;
        else if (*cursor != '+' && *cursor != '(' && *cursor != ')' &&
                 *cursor != '-' && !isspace((unsigned char)*cursor)) return 0;
    }
    return digits == 10 || digits == 11;
}

static void handle_seller_profile(int client, struct json_object *body, int64_t user_id) {
    const char *cpf = json_string(body, "cpf");
    const char *birth_date = json_string(body, "birthDate");
    const char *phone = json_string(body, "phone");
    const char *interests = json_string(body, "interests");
    if (cpf == NULL || birth_date == NULL || phone == NULL || interests == NULL ||
        !valid_cpf(cpf) || !valid_birth_date(birth_date) || !valid_phone(phone) ||
        strlen(interests) < 2 || strlen(interests) > 60) {
        respond_error(client, "400 Bad Request",
            "Confira o CPF, a data de nascimento, o telefone com DDD e a categoria.");
        return;
    }
    sqlite3_stmt *statement = NULL;
    const char *sql =
        "INSERT INTO seller_profiles(user_id,birth_date,phone,interests) VALUES(?,?,?,?) "
        "ON CONFLICT(user_id) DO UPDATE SET birth_date=excluded.birth_date,"
        "phone=excluded.phone,interests=excluded.interests";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível salvar o cadastro de vendedor.");
        return;
    }
    sqlite3_bind_int64(statement, 1, user_id);
    sqlite3_bind_text(statement, 2, birth_date, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, phone, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, interests, -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        respond_error(client, "500 Internal Server Error", "Não foi possível salvar o cadastro de vendedor.");
        return;
    }
    struct json_object *response = json_object_new_object();
    struct json_object *profile = json_object_new_object();
    json_object_object_add(profile, "birthDate", json_object_new_string(birth_date));
    json_object_object_add(profile, "phone", json_object_new_string(phone));
    json_object_object_add(profile, "interests", json_object_new_string(interests));
    json_object_object_add(response, "profile", profile);
    respond_json(client, "200 OK", response, NULL);
}

static void handle_seller_dashboard(int client, int64_t user_id) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, "SELECT COUNT(*) FROM products WHERE seller_id=? AND active=1",
                           -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível carregar o painel do vendedor.");
        return;
    }
    sqlite3_bind_int64(statement, 1, user_id);
    int result = sqlite3_step(statement);
    int64_t active_products = result == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : 0;
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW) {
        respond_error(client, "500 Internal Server Error", "Não foi possível carregar o painel do vendedor.");
        return;
    }

    const char *summary_sql =
        "SELECT COUNT(DISTINCT order_items.order_id),"
        "COALESCE(SUM(CASE WHEN orders.status='paid' "
        "AND orders.payment_status NOT IN ('refunded','charged_back') "
        "THEN (order_items.unit_price_cents+order_items.shipping_cents)*order_items.quantity ELSE 0 END),0) "
        "FROM order_items JOIN orders ON orders.id=order_items.order_id "
        "WHERE order_items.seller_id=?";
    if (sqlite3_prepare_v2(database, summary_sql, -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível carregar o painel do vendedor.");
        return;
    }
    sqlite3_bind_int64(statement, 1, user_id);
    result = sqlite3_step(statement);
    int64_t orders_received = result == SQLITE_ROW ? sqlite3_column_int64(statement, 0) : 0;
    int64_t revenue_cents = result == SQLITE_ROW ? sqlite3_column_int64(statement, 1) : 0;
    sqlite3_finalize(statement);
    if (result != SQLITE_ROW) {
        respond_error(client, "500 Internal Server Error", "Não foi possível carregar o painel do vendedor.");
        return;
    }

    const char *monthly_sql =
        "SELECT strftime('%Y-%m',orders.created_at),"
        "SUM((order_items.unit_price_cents+order_items.shipping_cents)*order_items.quantity) "
        "FROM order_items JOIN orders ON orders.id=order_items.order_id "
        "WHERE order_items.seller_id=? AND orders.status='paid' "
        "AND orders.payment_status NOT IN ('refunded','charged_back') "
        "AND orders.created_at>=date('now','start of month','-5 months') "
        "GROUP BY strftime('%Y-%m',orders.created_at) ORDER BY 1";
    if (sqlite3_prepare_v2(database, monthly_sql, -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível carregar o gráfico do painel.");
        return;
    }
    sqlite3_bind_int64(statement, 1, user_id);
    struct json_object *response = json_object_new_object();
    struct json_object *months = json_object_new_array();
    json_object_object_add(response, "activeProducts", json_object_new_int64(active_products));
    json_object_object_add(response, "ordersReceived", json_object_new_int64(orders_received));
    json_object_object_add(response, "revenueCents", json_object_new_int64(revenue_cents));
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        struct json_object *month = json_object_new_object();
        json_object_object_add(month, "month",
            json_object_new_string((const char *)sqlite3_column_text(statement, 0)));
        json_object_object_add(month, "revenueCents",
            json_object_new_int64(sqlite3_column_int64(statement, 1)));
        json_object_array_add(months, month);
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE) {
        json_object_put(response);
        respond_error(client, "500 Internal Server Error", "Não foi possível carregar o gráfico do painel.");
        return;
    }
    json_object_object_add(response, "monthlyRevenue", months);
    respond_json(client, "200 OK", response, NULL);
}

static void decode_query(const char *path, char *result, size_t capacity) {
    const char *query = strchr(path, '?');
    if (query == NULL) {
        result[0] = '\0';
        return;
    }
    query++;
    const char *key = strstr(query, "q=");
    if (key == NULL) {
        result[0] = '\0';
        return;
    }
    key += 2;
    size_t used = 0;
    while (*key != '\0' && *key != '&' && used + 1 < capacity) {
        if (*key == '%' && isxdigit((unsigned char)key[1]) && isxdigit((unsigned char)key[2])) {
            unsigned int value;
            sscanf(key + 1, "%2x", &value);
            result[used++] = (char)value;
            key += 3;
        } else {
            result[used++] = *key == '+' ? ' ' : *key;
            key++;
        }
    }
    result[used] = '\0';
}

static int decode_query_parameter(const char *path, const char *parameter,
                                  char *result, size_t capacity) {
    const char *query = strchr(path, '?');
    if (query == NULL || capacity == 0) return -1;
    query++;
    size_t parameter_length = strlen(parameter);
    while (*query != '\0') {
        const char *value = strstr(query, parameter);
        if (value == NULL || (value != query && value[-1] != '&')) return -1;
        value += parameter_length;
        size_t used = 0;
        while (*value != '\0' && *value != '&' && used + 1 < capacity) {
            if (*value == '%' && isxdigit((unsigned char)value[1]) &&
                isxdigit((unsigned char)value[2])) {
                unsigned int byte = 0;
                if (sscanf(value + 1, "%2x", &byte) != 1 || byte == 0) return -1;
                result[used++] = (char)byte;
                value += 3;
            } else if (*value == '+') {
                result[used++] = ' ';
                value++;
            } else {
                result[used++] = *value++;
            }
        }
        if (*value != '\0' && *value != '&') return -1;
        result[used] = '\0';
        return used == 0 ? -1 : 0;
    }
    return -1;
}

static const char *oauth_cookie(const char *request, char *value, size_t capacity) {
    const char *line = strstr(request, "\r\n");
    if (line == NULL) return NULL;
    line += 2;
    while (*line != '\0' && strncmp(line, "\r\n", 2) != 0) {
        const char *end = strstr(line, "\r\n");
        if (end == NULL) return NULL;
        if (strncasecmp(line, "Cookie:", 7) == 0) {
            const char *cookie = line + 7;
            while (cookie < end) {
                while (cookie < end && (*cookie == ' ' || *cookie == ';')) cookie++;
                const char *equals = strchr(cookie, '=');
                if (equals == NULL || equals >= end) break;
                if ((size_t)(equals - cookie) == strlen("vitrine_oauth_state") &&
                    strncmp(cookie, "vitrine_oauth_state", (size_t)(equals - cookie)) == 0) {
                    const char *start = equals + 1;
                    const char *finish = start;
                    while (finish < end && *finish != ';') finish++;
                    size_t length = (size_t)(finish - start);
                    if (length + 1 > capacity) return NULL;
                    memcpy(value, start, length);
                    value[length] = '\0';
                    return value;
                }
                cookie = strchr(equals, ';');
                if (cookie == NULL || cookie >= end) break;
                cookie++;
            }
        }
        line = end + 2;
    }
    return NULL;
}

static const char *oauth_env(const char *key) {
    const char *value = getenv(key);
    return value == NULL || value[0] == '\0' ? NULL : value;
}

static int google_oauth_configured(void) {
    return oauth_env("GOOGLE_CLIENT_ID") != NULL &&
           oauth_env("GOOGLE_CLIENT_SECRET") != NULL &&
           oauth_env("GOOGLE_REDIRECT_URI") != NULL;
}

static int hash_hex(const char *value, char *output) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_length = 0;
    if (EVP_Digest(value, strlen(value), digest, &digest_length, EVP_sha256(), NULL) != 1) return -1;
    bytes_to_hex(digest, digest_length, output);
    OPENSSL_cleanse(digest, sizeof(digest));
    return 0;
}

static char *url_escape(CURL *curl, const char *value) {
    return curl_easy_escape(curl, value, (int)strlen(value));
}

static void handle_google_start(int client) {
    if (!google_oauth_configured()) {
        respond_redirect(client, "/?auth=google-unavailable", NULL);
        return;
    }
    unsigned char random_state[OAUTH_STATE_BYTES];
    char state[OAUTH_STATE_BYTES * 2 + 1];
    char state_hash[EVP_MAX_MD_SIZE * 2 + 1];
    if (RAND_bytes(random_state, sizeof(random_state)) != 1) {
        respond_error(client, "500 Internal Server Error", "Não foi possível iniciar o acesso com Google.");
        return;
    }
    bytes_to_hex(random_state, sizeof(random_state), state);
    OPENSSL_cleanse(random_state, sizeof(random_state));
    if (hash_hex(state, state_hash) != 0 ||
        execute_sql("DELETE FROM oauth_states WHERE expires_at < strftime('%s','now')") != 0) {
        respond_error(client, "500 Internal Server Error", "Não foi possível iniciar o acesso com Google.");
        return;
    }
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, "INSERT INTO oauth_states(state_hash,expires_at) VALUES(?,?)",
                           -1, &statement, NULL) != SQLITE_OK) {
        respond_error(client, "500 Internal Server Error", "Não foi possível iniciar o acesso com Google.");
        return;
    }
    sqlite3_bind_text(statement, 1, state_hash, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, (int64_t)time(NULL) + OAUTH_STATE_MAX_AGE);
    int saved = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (saved != SQLITE_DONE) {
        respond_error(client, "500 Internal Server Error", "Não foi possível iniciar o acesso com Google.");
        return;
    }

    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        respond_error(client, "500 Internal Server Error", "Não foi possível iniciar o acesso com Google.");
        return;
    }
    char *client_id = url_escape(curl, oauth_env("GOOGLE_CLIENT_ID"));
    char *redirect_uri = url_escape(curl, oauth_env("GOOGLE_REDIRECT_URI"));
    char location[4096];
    int length = client_id == NULL || redirect_uri == NULL ? -1 :
        snprintf(location, sizeof(location),
            "https://accounts.google.com/o/oauth2/v2/auth?client_id=%s&redirect_uri=%s"
            "&response_type=code&scope=openid%%20email%%20profile&state=%s&prompt=select_account",
            client_id, redirect_uri, state);
    curl_free(client_id);
    curl_free(redirect_uri);
    curl_easy_cleanup(curl);
    if (length < 0 || (size_t)length >= sizeof(location)) {
        respond_error(client, "500 Internal Server Error", "A configuração de acesso com Google é inválida.");
        return;
    }
    char cookie[256];
    snprintf(cookie, sizeof(cookie),
        "Set-Cookie: vitrine_oauth_state=%s; Path=/auth/google/callback; HttpOnly; SameSite=Lax; Max-Age=%d%s\r\n",
        state, OAUTH_STATE_MAX_AGE, secure_cookie ? "; Secure" : "");
    respond_redirect(client, location, cookie);
}

static int consume_oauth_state(const char *state, const char *cookie_state) {
    if (state == NULL || cookie_state == NULL || strlen(state) != OAUTH_STATE_BYTES * 2 ||
        strlen(cookie_state) != OAUTH_STATE_BYTES * 2 ||
        CRYPTO_memcmp(state, cookie_state, OAUTH_STATE_BYTES * 2) != 0) return 0;
    for (size_t i = 0; i < OAUTH_STATE_BYTES * 2; i++) {
        if (!isxdigit((unsigned char)state[i])) return 0;
    }
    char digest[EVP_MAX_MD_SIZE * 2 + 1];
    if (hash_hex(state, digest) != 0 || execute_sql("BEGIN IMMEDIATE") != 0) return 0;
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database,
            "DELETE FROM oauth_states WHERE state_hash=? AND expires_at>?",
            -1, &statement, NULL) != SQLITE_OK) {
        execute_sql("ROLLBACK");
        return 0;
    }
    sqlite3_bind_text(statement, 1, digest, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, (int64_t)time(NULL));
    int result = sqlite3_step(statement);
    int consumed = result == SQLITE_DONE && sqlite3_changes(database) == 1;
    sqlite3_finalize(statement);
    if (consumed) {
        if (execute_sql("COMMIT") != 0) {
            execute_sql("ROLLBACK");
            return 0;
        }
    } else {
        execute_sql("ROLLBACK");
    }
    return consumed;
}

static int authenticate_google_user(const char *google_id, const char *email, const char *name,
                                    int64_t *user_id) {
    if (!valid_email(email)) return -1;
    if (execute_sql("BEGIN IMMEDIATE") != 0) return -1;
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, "SELECT id,google_sub FROM users WHERE google_sub=?",
                           -1, &statement, NULL) != SQLITE_OK) {
        execute_sql("ROLLBACK");
        return -1;
    }
    sqlite3_bind_text(statement, 1, google_id, -1, SQLITE_TRANSIENT);
    int result = sqlite3_step(statement);
    if (result == SQLITE_ROW) {
        *user_id = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        if (execute_sql("COMMIT") != 0) {
            execute_sql("ROLLBACK");
            return -1;
        }
        return 0;
    }
    sqlite3_finalize(statement);
    if (result != SQLITE_DONE ||
        sqlite3_prepare_v2(database, "SELECT id,google_sub FROM users WHERE email=?",
                           -1, &statement, NULL) != SQLITE_OK) {
        execute_sql("ROLLBACK");
        return -1;
    }
    sqlite3_bind_text(statement, 1, email, -1, SQLITE_TRANSIENT);
    result = sqlite3_step(statement);
    if (result == SQLITE_ROW) {
        *user_id = sqlite3_column_int64(statement, 0);
        const char *linked_google_id = (const char *)sqlite3_column_text(statement, 1);
        int already_linked = linked_google_id != NULL && linked_google_id[0] != '\0';
        sqlite3_finalize(statement);
        if (already_linked) {
            execute_sql("ROLLBACK");
            return -2;
        }
        if (sqlite3_prepare_v2(database, "UPDATE users SET google_sub=? WHERE id=? AND google_sub IS NULL",
                               -1, &statement, NULL) != SQLITE_OK) {
            execute_sql("ROLLBACK");
            return -1;
        }
        sqlite3_bind_text(statement, 1, google_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(statement, 2, *user_id);
        result = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (result != SQLITE_DONE || sqlite3_changes(database) != 1) {
            execute_sql("ROLLBACK");
            return -1;
        }
    } else {
        sqlite3_finalize(statement);
        if (result != SQLITE_DONE) {
            execute_sql("ROLLBACK");
            return -1;
        }
        unsigned char salt[16], random_password[32];
        char salt_hex[33], password_hex[sizeof(random_password) * 2 + 1], password_digest[65];
        if (RAND_bytes(salt, sizeof(salt)) != 1 ||
            RAND_bytes(random_password, sizeof(random_password)) != 1) {
            execute_sql("ROLLBACK");
            return -1;
        }
        bytes_to_hex(salt, sizeof(salt), salt_hex);
        bytes_to_hex(random_password, sizeof(random_password), password_hex);
        OPENSSL_cleanse(random_password, sizeof(random_password));
        if (password_hash(password_hex, salt_hex, password_digest) != 0 ||
            sqlite3_prepare_v2(database,
                "INSERT INTO users(name,email,password_salt,password_hash,google_sub) VALUES(?,?,?,?,?)",
                -1, &statement, NULL) != SQLITE_OK) {
            OPENSSL_cleanse(password_hex, sizeof(password_hex));
            execute_sql("ROLLBACK");
            return -1;
        }
        sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, email, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 3, salt_hex, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 4, password_digest, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 5, google_id, -1, SQLITE_TRANSIENT);
        result = sqlite3_step(statement);
        sqlite3_finalize(statement);
        OPENSSL_cleanse(password_hex, sizeof(password_hex));
        OPENSSL_cleanse(password_digest, sizeof(password_digest));
        if (result != SQLITE_DONE) {
            execute_sql("ROLLBACK");
            return -1;
        }
        *user_id = sqlite3_last_insert_rowid(database);
    }
    if (execute_sql("COMMIT") != 0) {
        execute_sql("ROLLBACK");
        return -1;
    }
    return 0;
}

static void handle_google_callback(int client, const char *request, const char *path) {
    char clear_cookie[] =
        "Set-Cookie: vitrine_oauth_state=; Path=/auth/google/callback; HttpOnly; SameSite=Lax; Max-Age=0\r\n";
    if (!google_oauth_configured()) {
        respond_redirect(client, "/?auth=google-unavailable", clear_cookie);
        return;
    }
    char state[OAUTH_STATE_BYTES * 2 + 1], cookie_state[OAUTH_STATE_BYTES * 2 + 1];
    if (decode_query_parameter(path, "state=", state, sizeof(state)) != 0 ||
        oauth_cookie(request, cookie_state, sizeof(cookie_state)) == NULL ||
        !consume_oauth_state(state, cookie_state)) {
        respond_redirect(client, "/?auth=google-error", clear_cookie);
        return;
    }
    char provider_error[256];
    if (decode_query_parameter(path, "error=", provider_error, sizeof(provider_error)) == 0) {
        respond_redirect(client, "/?auth=google-error", clear_cookie);
        return;
    }
    char code[2048];
    if (decode_query_parameter(path, "code=", code, sizeof(code)) != 0) {
        respond_redirect(client, "/?auth=google-error", clear_cookie);
        return;
    }
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        respond_redirect(client, "/?auth=google-error", clear_cookie);
        return;
    }
    char *escaped_code = url_escape(curl, code);
    char *escaped_client = url_escape(curl, oauth_env("GOOGLE_CLIENT_ID"));
    char *escaped_secret = url_escape(curl, oauth_env("GOOGLE_CLIENT_SECRET"));
    char *escaped_redirect = url_escape(curl, oauth_env("GOOGLE_REDIRECT_URI"));
    char token_form[8192];
    int form_length = escaped_code == NULL || escaped_client == NULL ||
                      escaped_secret == NULL || escaped_redirect == NULL ? -1 :
        snprintf(token_form, sizeof(token_form),
            "code=%s&client_id=%s&client_secret=%s&redirect_uri=%s&grant_type=authorization_code",
            escaped_code, escaped_client, escaped_secret, escaped_redirect);
    curl_free(escaped_code);
    curl_free(escaped_client);
    curl_free(escaped_secret);
    curl_free(escaped_redirect);
    curl_easy_cleanup(curl);
    if (form_length < 0 || (size_t)form_length >= sizeof(token_form)) {
        respond_redirect(client, "/?auth=google-error", clear_cookie);
        return;
    }
    struct json_object *token_response = NULL;
    if (http_post_form("https://oauth2.googleapis.com/token", token_form, &token_response) != 0) {
        OPENSSL_cleanse(token_form, sizeof(token_form));
        respond_redirect(client, "/?auth=google-error", clear_cookie);
        return;
    }
    OPENSSL_cleanse(token_form, sizeof(token_form));
    const char *access_token = json_string(token_response, "access_token");
    char authorization[8192];
    int auth_length = access_token == NULL ? -1 :
        snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s", access_token);
    if (auth_length < 0 || (size_t)auth_length >= sizeof(authorization)) {
        json_object_put(token_response);
        respond_redirect(client, "/?auth=google-error", clear_cookie);
        return;
    }
    struct json_object *profile = NULL;
    int profile_result = http_get_json("https://openidconnect.googleapis.com/v1/userinfo",
                                       authorization, &profile);
    OPENSSL_cleanse(authorization, sizeof(authorization));
    json_object_put(token_response);
    if (profile_result != 0) {
        respond_redirect(client, "/?auth=google-error", clear_cookie);
        return;
    }
    const char *google_id = json_string(profile, "sub");
    const char *email = json_string(profile, "email");
    const char *name = json_string(profile, "name");
    struct json_object *verified_value = NULL;
    int verified = json_object_object_get_ex(profile, "email_verified", &verified_value) &&
                   json_object_get_type(verified_value) == json_type_boolean &&
                   json_object_get_boolean(verified_value);
    int valid_profile = google_id != NULL && strlen(google_id) > 0 && strlen(google_id) < 256 &&
                        email != NULL && valid_email(email) && verified &&
                        (name == NULL || strlen(name) <= 80);
    int64_t user_id = 0;
    int login_result = valid_profile
        ? authenticate_google_user(google_id, email, name == NULL || name[0] == '\0' ? email : name,
                                   &user_id)
        : -1;
    json_object_put(profile);
    if (login_result != 0) {
        respond_redirect(client, login_result == -2
            ? "/?auth=google-conflict" : "/?auth=google-error", clear_cookie);
        return;
    }
    char session_cookie[512];
    if (store_session(user_id, session_cookie, sizeof(session_cookie)) != 0) {
        respond_redirect(client, "/?auth=google-error", clear_cookie);
        return;
    }
    char cookies[800];
    snprintf(cookies, sizeof(cookies), "%s%s", session_cookie, clear_cookie);
    respond_redirect(client, "/?auth=google-success", cookies);
}

static size_t content_length(const char *headers, size_t header_length, int *valid) {
    const char *line = strstr(headers, "\r\n");
    const char *end = headers + header_length;
    *valid = 1;
    if (line == NULL) return 0;
    line += 2;
    while (line < end) {
        const char *line_end = strstr(line, "\r\n");
        if (line_end == NULL || line_end > end) break;
        if ((size_t)(line_end - line) >= 15 && strncasecmp(line, "Content-Length:", 15) == 0) {
            const char *value = line + 15;
            while (value < line_end && isspace((unsigned char)*value)) value++;
            char *number_end = NULL;
            errno = 0;
            unsigned long parsed = strtoul(value, &number_end, 10);
            if (errno != 0 || number_end == value || number_end > line_end ||
                parsed > REQUEST_CAPACITY) {
                *valid = 0;
                return 0;
            }
            return (size_t)parsed;
        }
        line = line_end + 2;
    }
    return 0;
}

static void handle_client(int client) {
    char request[REQUEST_CAPACITY + 1];
    size_t received = 0, body_size = 0;
    char *headers_end = NULL;
    int length_valid = 1;
    while (received < REQUEST_CAPACITY) {
        ssize_t count = recv(client, request + received, REQUEST_CAPACITY - received, 0);
        if (count <= 0) return;
        received += (size_t)count;
        request[received] = '\0';
        headers_end = strstr(request, "\r\n\r\n");
        if (headers_end == NULL) continue;
        size_t header_size = (size_t)(headers_end - request) + 2;
        body_size = content_length(request, header_size, &length_valid);
        if (!length_valid) {
            respond_error(client, "413 Payload Too Large", "Requisição inválida ou muito grande.");
            return;
        }
        size_t body_received = received - (size_t)(headers_end + 4 - request);
        if (body_received >= body_size) break;
    }
    if (headers_end == NULL || received >= REQUEST_CAPACITY) {
        respond_error(client, "400 Bad Request", "Cabeçalho HTTP inválido.");
        return;
    }
    char method[8] = {0}, path[1024] = {0}, original_path[1024] = {0};
    if (sscanf(request, "%7s %1023s", method, path) != 2) {
        respond_error(client, "400 Bad Request", "Requisição inválida.");
        return;
    }
    snprintf(original_path, sizeof(original_path), "%s", path);
    int64_t user_id = 0;
    char user_name[256] = {0}, user_email[256] = {0};
    int authenticated = get_authenticated_user(request, &user_id, user_name, sizeof(user_name),
                                                user_email, sizeof(user_email));
    char *query = strchr(path, '?');
    char search[512];
    decode_query(path, search, sizeof(search));
    if (query != NULL) *query = '\0';

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/payments/config") == 0) {
        struct json_object *response = json_object_new_object();
        json_object_object_add(response, "enabled", json_object_new_boolean(mercadopago_configured()));
        respond_json(client, "200 OK", response, NULL);
        return;
    }
    if (strcmp(method, "POST") == 0 &&
        strcmp(path, "/api/payments/mercadopago/webhook") == 0) {
        const char *body = headers_end + 4;
        size_t actual_body = received - (size_t)(body - request);
        handle_mercadopago_webhook(client, request, original_path, body,
                                   body_size <= actual_body ? body_size : 0);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/auth/google") == 0) {
        struct json_object *response = json_object_new_object();
        json_object_object_add(response, "enabled", json_object_new_boolean(google_oauth_configured()));
        respond_json(client, "200 OK", response, NULL);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/auth/google") == 0) {
        handle_google_start(client);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/auth/google/callback") == 0) {
        handle_google_callback(client, request, original_path);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/products") == 0) {
        handle_products(client, search);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/me") == 0) {
        handle_me(client, authenticated ? user_id : 0, user_name, user_email);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/seller-dashboard") == 0) {
        if (!authenticated) {
            respond_error(client, "401 Unauthorized", "Entre na sua conta para acessar o painel do vendedor.");
            return;
        }
        handle_seller_dashboard(client, user_id);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/seller-orders") == 0) {
        if (!authenticated) {
            respond_error(client, "401 Unauthorized", "Entre na sua conta para ver pedidos de clientes.");
            return;
        }
        handle_seller_orders(client, user_id);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/cart") == 0) {
        if (!authenticated) {
            respond_error(client, "401 Unauthorized", "Entre na sua conta para acessar a sacola.");
            return;
        }
        struct json_object *response = cart_json(user_id);
        if (response == NULL) respond_error(client, "500 Internal Server Error", "Não foi possível carregar a sacola.");
        else respond_json(client, "200 OK", response, NULL);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/my-products") == 0) {
        if (!authenticated) {
            respond_error(client, "401 Unauthorized", "Entre na sua conta para ver seus anúncios.");
            return;
        }
        handle_my_products(client, user_id);
        return;
    }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/orders") == 0) {
        if (!authenticated) {
            respond_error(client, "401 Unauthorized", "Entre na sua conta para ver seus pedidos.");
            return;
        }
        handle_orders(client, user_id);
        return;
    }
    if ((strcmp(path, "/api/register") == 0 || strcmp(path, "/api/login") == 0) &&
        strcmp(method, "POST") == 0) {
        const char *body = headers_end + 4;
        size_t actual_body = received - (size_t)(body - request);
        if (body_size == 0 || body_size > actual_body) {
            respond_error(client, "400 Bad Request", "O corpo da requisição está vazio ou incompleto.");
            return;
        }
        struct json_object *json = parse_json(body, body_size);
        if (json == NULL) {
            respond_error(client, "400 Bad Request", "O corpo da requisição não é um JSON válido.");
            return;
        }
        if (strcmp(path, "/api/register") == 0) handle_register(client, json);
        else handle_login(client, json);
        json_object_put(json);
        return;
    }
    if (strcmp(path, "/api/seller-profile") == 0 && strcmp(method, "POST") == 0) {
        if (!authenticated) {
            respond_error(client, "401 Unauthorized", "Entre na sua conta para criar seu perfil de vendedor.");
            return;
        }
        const char *body = headers_end + 4;
        size_t actual_body = received - (size_t)(body - request);
        if (body_size == 0 || body_size > actual_body) {
            respond_error(client, "400 Bad Request", "O cadastro enviado está vazio ou incompleto.");
            return;
        }
        struct json_object *json = parse_json(body, body_size);
        if (json == NULL) {
            respond_error(client, "400 Bad Request", "O cadastro enviado não é um JSON válido.");
            return;
        }
        handle_seller_profile(client, json, user_id);
        json_object_put(json);
        return;
    }
    if (strcmp(path, "/api/logout") == 0 && strcmp(method, "POST") == 0) {
        handle_logout(client, authenticated ? user_id : 0, request);
        return;
    }
    if (strncmp(path, "/api/products/", 14) == 0 && strcmp(method, "POST") == 0) {
        if (!authenticated) {
            respond_error(client, "401 Unauthorized", "Entre na sua conta para avaliar produtos.");
            return;
        }
        char *end = NULL;
        long product_id = strtol(path + 14, &end, 10);
        if (end == path + 14 || product_id < 1 || strcmp(end, "/reviews") != 0) {
            respond_error(client, "404 Not Found", "Rota de avaliação não encontrada.");
            return;
        }
        const char *body = headers_end + 4;
        size_t actual_body = received - (size_t)(body - request);
        struct json_object *json = body_size > 0 && body_size <= actual_body
            ? parse_json(body, body_size) : NULL;
        if (json == NULL) {
            respond_error(client, "400 Bad Request", "A avaliação enviada não é um JSON válido.");
            return;
        }
        handle_product_review(client, (int64_t)product_id, user_id, json);
        json_object_put(json);
        return;
    }
    if (strncmp(path, "/api/products/", 14) == 0 && strcmp(method, "DELETE") == 0) {
        if (!authenticated) {
            respond_error(client, "401 Unauthorized", "Entre na sua conta para remover seus produtos.");
            return;
        }
        char *end = NULL;
        long product_id = strtol(path + 14, &end, 10);
        if (end == path + 14 || *end != '\0' || product_id < 1) {
            respond_error(client, "400 Bad Request", "Identificador de produto inválido.");
            return;
        }
        if (execute_sql("BEGIN IMMEDIATE") != 0) {
            respond_error(client, "500 Internal Server Error", "Não foi possível remover o anúncio.");
            return;
        }
        sqlite3_stmt *statement = NULL;
        if (sqlite3_prepare_v2(database, "UPDATE products SET active=0 WHERE id=? AND seller_id=? AND active=1",
                               -1, &statement, NULL) != SQLITE_OK) {
            execute_sql("ROLLBACK");
            respond_error(client, "500 Internal Server Error", "Não foi possível remover o produto.");
            return;
        }
        sqlite3_bind_int64(statement, 1, product_id);
        sqlite3_bind_int64(statement, 2, user_id);
        int result = sqlite3_step(statement);
        int changed = sqlite3_changes(database);
        sqlite3_finalize(statement);
        if (result != SQLITE_DONE || changed == 0) {
            execute_sql("ROLLBACK");
            respond_error(client, "404 Not Found", "Produto não encontrado na sua loja.");
            return;
        }
        if (sqlite3_prepare_v2(database, "DELETE FROM cart_items WHERE product_id=?",
                               -1, &statement, NULL) != SQLITE_OK) {
            execute_sql("ROLLBACK");
            respond_error(client, "500 Internal Server Error", "Anúncio removido, mas não foi possível atualizar as sacolas.");
            return;
        }
        sqlite3_bind_int64(statement, 1, product_id);
        result = sqlite3_step(statement);
        sqlite3_finalize(statement);
        if (result != SQLITE_DONE || execute_sql("COMMIT") != 0) {
            execute_sql("ROLLBACK");
            respond_error(client, "500 Internal Server Error", "Anúncio removido, mas não foi possível atualizar as sacolas.");
            return;
        }
        struct json_object *response = json_object_new_object();
        json_object_object_add(response, "ok", json_object_new_boolean(1));
        respond_json(client, "200 OK", response, NULL);
        return;
    }
    if (strcmp(path, "/api/products") == 0 && strcmp(method, "POST") == 0) {
        if (!authenticated) {
            respond_error(client, "401 Unauthorized", "Entre na sua conta para publicar produtos.");
            return;
        }
        const char *body = headers_end + 4;
        size_t actual_body = received - (size_t)(body - request);
        if (body_size == 0 || body_size > actual_body) {
            respond_error(client, "400 Bad Request", "O corpo da requisição está vazio ou incompleto.");
            return;
        }
        struct json_object *json = parse_json(body, body_size);
        if (json == NULL) {
            respond_error(client, "400 Bad Request", "O corpo da requisição não é um JSON válido.");
            return;
        }
        handle_create_product(client, json, user_id);
        json_object_put(json);
        return;
    }
    if (strcmp(path, "/api/cart") == 0 && strcmp(method, "POST") == 0) {
        if (!authenticated) {
            respond_error(client, "401 Unauthorized", "Entre na sua conta antes de adicionar produtos à sacola.");
            return;
        }
        const char *body = headers_end + 4;
        size_t actual_body = received - (size_t)(body - request);
        struct json_object *json = body_size > 0 && body_size <= actual_body
            ? parse_json(body, body_size) : NULL;
        if (json == NULL) {
            respond_error(client, "400 Bad Request", "O corpo da requisição não é um JSON válido.");
            return;
        }
        handle_add_cart(client, json, user_id);
        json_object_put(json);
        return;
    }
    if (strcmp(path, "/api/orders") == 0 && strcmp(method, "POST") == 0) {
        if (!authenticated) {
            respond_error(client, "401 Unauthorized", "Entre na sua conta antes de continuar.");
            return;
        }
        const char *body = headers_end + 4;
        size_t actual_body = received - (size_t)(body - request);
        struct json_object *json = body_size > 0 && body_size <= actual_body
            ? parse_json(body, body_size) : NULL;
        if (json == NULL) {
            respond_error(client, "400 Bad Request", "Informe os dados para entrega do pedido.");
            return;
        }
        handle_order(client, json, user_id);
        json_object_put(json);
        return;
    }
    if (strncmp(path, "/api/seller-orders/", strlen("/api/seller-orders/")) == 0 &&
        strcmp(method, "POST") == 0) {
        if (!authenticated) {
            respond_error(client, "401 Unauthorized", "Entre na sua conta para atualizar uma entrega.");
            return;
        }
        char *end = NULL;
        int64_t order_id = strtoll(path + strlen("/api/seller-orders/"), &end, 10);
        if (end == path + strlen("/api/seller-orders/") || *end != '\0' || order_id < 1) {
            respond_error(client, "404 Not Found", "Pedido não encontrado.");
            return;
        }
        const char *body = headers_end + 4;
        size_t actual_body = received - (size_t)(body - request);
        struct json_object *json = body_size > 0 && body_size <= actual_body
            ? parse_json(body, body_size) : NULL;
        if (json == NULL) {
            respond_error(client, "400 Bad Request", "Os dados de entrega enviados são inválidos.");
            return;
        }
        handle_seller_order_update(client, json, user_id, order_id);
        json_object_put(json);
        return;
    }
    if (strncmp(path, "/api/cart/", strlen("/api/cart/")) == 0 && strcmp(method, "DELETE") == 0) {
        if (!authenticated) {
            respond_error(client, "401 Unauthorized", "Entre na sua conta para acessar a sacola.");
            return;
        }
        char *end = NULL;
        long product_id = strtol(path + strlen("/api/cart/"), &end, 10);
        if (end == path + strlen("/api/cart/") || *end != '\0' || product_id < 1) {
            respond_error(client, "404 Not Found", "Rota não encontrada.");
            return;
        }
        handle_delete_cart(client, product_id, user_id);
        return;
    }
    if (strncmp(path, "/api/", 5) == 0) {
        respond_error(client, "404 Not Found", "Rota da API não encontrada.");
        return;
    }
    if (strcmp(method, "GET") != 0) {
        respond_error(client, "405 Method Not Allowed", "Método não permitido.");
        return;
    }
    const char *filename = NULL;
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) filename = "index.html";
    else if (strcmp(path, "/app.css") == 0) filename = "app.css";
    else if (strcmp(path, "/app.js") == 0) filename = "app.js";
    else if (strcmp(path, "/login-shopping-bag.png") == 0) filename = "login-shopping-bag.png";
    else if (strcmp(path, "/shope-pago-logo.png") == 0) filename = "shope-pago-logo.png";
    if (filename == NULL) {
        RESPOND_TEXT(client, "404 Not Found", "Não encontrado.");
        return;
    }
    FILE *file = fopen(filename, "rb");
    if (file == NULL || fseek(file, 0, SEEK_END) != 0) {
        if (file != NULL) fclose(file);
        RESPOND_TEXT(client, "500 Internal Server Error", "Arquivo da loja indisponível.");
        return;
    }
    long file_size = ftell(file);
    if (file_size < 0 || file_size > 1024 * 1024 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        RESPOND_TEXT(client, "500 Internal Server Error", "Arquivo inválido.");
        return;
    }
    char *content = malloc((size_t)file_size);
    if (content == NULL || fread(content, 1, (size_t)file_size, file) != (size_t)file_size) {
        free(content);
        fclose(file);
        RESPOND_TEXT(client, "500 Internal Server Error", "Falha ao ler arquivo.");
        return;
    }
    fclose(file);
    const char *type = strcmp(filename, "app.css") == 0 ? "text/css" :
                       strcmp(filename, "app.js") == 0 ? "application/javascript" :
                       (strcmp(filename, "login-shopping-bag.png") == 0 ||
                        strcmp(filename, "shope-pago-logo.png") == 0) ? "image/png" : "text/html";
    respond(client, "200 OK", type, content, (size_t)file_size, NULL);
    free(content);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    secure_cookie = getenv("COOKIE_SECURE") != NULL &&
                    strcmp(getenv("COOKIE_SECURE"), "1") == 0;
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        fprintf(stderr, "Não foi possível inicializar o cliente HTTPS.\n");
        return 1;
    }
    if (initialize_database() != 0) return 1;
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        perror("socket");
        sqlite3_close(database);
        return 1;
    }
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    address.sin_port = htons(PORT);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(server, 32) < 0) {
        perror("listen");
        close(server);
        sqlite3_close(database);
        return 1;
    }
    printf("Loja pronta na porta %d\n", PORT);
    fflush(stdout);
    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }
        struct timeval timeout = {10, 0};
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
        handle_client(client);
        close(client);
    }
}

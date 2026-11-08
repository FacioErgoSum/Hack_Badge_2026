// -----------------------------------------------------------------------------
// net - Wi-Fi, drawing uploads, the live view and the gallery download.
// See net.h for the shape of it. Protocol, all JSON over HTTPS to BADGE_SERVER
// with the header X-Badge-Key: BADGE_UPLOAD_KEY
//
//   POST /api/drawings              {id, p}               a finished drawing
//   POST /api/live                  {id, from, p, mode}   points since `from`
//   GET  /api/drawings?format=badge&limit=N   {items: [{t, p, s}]}
//   GET  /api/canvas?cv=C&from=N    {cv, from, s: [...]}   lines from the website
//   GET  /api/canvas?part=template  {tv, m}                the tracing template
//
// p is the points as little-endian int16 x, y pairs, base64'd; s a list of
// those, one per line. Every /api/live reply also carries the canvas summary
// {cv, cn, wn, tv, open}, which is how the badge knows to fetch.
// -----------------------------------------------------------------------------
#include "net.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"

#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#else
#warning "src/wifi_secrets.h is missing: copy wifi_secrets.example.h and fill it in. Building without Wi-Fi."
#define WIFI_SSID        ""
#define WIFI_PASS        ""
#define BADGE_SERVER     ""
#define BADGE_UPLOAD_KEY ""
#endif

// --- Tunables -----------------------------------------------------------------
#define MAX_POINTS        60000     // per drawing; ~4 bytes each in PSRAM
#define MIN_DRAWING_PX    40        // shorter paths than this aren't uploaded
#define LIVE_MS           2000      // send new points this often while drawing
#define HEARTBEAT_MS      15000     // ...and check in this often regardless
#define LIVE_CHUNK        3000      // points per live post at most
#define GALLERY_LIMIT     8
#define RETRY_MIN_MS      5000
#define RETRY_MAX_MS      300000
#define HTTP_TIMEOUT_MS   20000
#define MAX_RESPONSE      (3 * 1024 * 1024)
#define SPOOL_DIR         "/spool"

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void* ps_malloc(size_t n)
{
  void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);
}

// =============================================================================
// State shared with the app
// =============================================================================
static SemaphoreHandle_t s_mtx;
static EventGroupHandle_t s_ev;
#define EV_ONLINE BIT0

static TaskHandle_t s_task;
static QueueHandle_t s_done_q;     // finished drawings, waiting to be written to flash
static volatile bool s_online, s_ev_uploaded, s_ev_connected;
static volatile net_mode_t s_mode = NET_MODE_DRAWING;
static volatile bool s_mode_changed;
static bool s_spool_ok;
static char s_badge[16];           // MAC-based badge id
static uint32_t s_seq;             // drawing counter, kept in NVS

// The drawing in progress. Points below s_frozen have gone (or are going) out
// on the live view, so they mustn't be merged into anymore.
static int16_t* s_pts;             // x, y pairs
static int s_n;
static char s_id[32];
static int s_live_from;            // the server has points [0, s_live_from)
static int s_frozen;

typedef struct {
  char id[32];
  int n;
  int16_t* xy;
} drawing_t;

// Template and open canvas. s_srv_* is the server's latest summary (from the
// live replies); the rest is what the badge holds.
static uint8_t* s_tpl;
static volatile bool s_tpl_changed;
static int s_tv = 0;
static net_stroke_t* s_strokes;
static int s_ns, s_ns_cap;
static int s_cv;
static volatile int s_epoch;
static volatile bool s_open;
static volatile int s_srv_cv, s_srv_cn, s_srv_wn, s_srv_tv;
#define TPL_BYTES ((NET_TPL_W / 8) * NET_TPL_H)

// Gallery
static volatile net_gallery_state_t s_gal_state;
static volatile bool s_gal_req;
static net_gallery_item_t s_gal[GALLERY_LIMIT];
static int s_gal_n;

bool net_online(void) { return s_online; }
bool net_configured(void) { return BADGE_SERVER[0] && !strstr(BADGE_SERVER, "YOUR-SITE"); }

void net_lock(void)   { xSemaphoreTake(s_mtx, portMAX_DELAY); }
void net_unlock(void) { xSemaphoreGive(s_mtx); }
const uint8_t* net_template(void) { return s_tpl; }
bool net_take_template_changed(void) { bool v = s_tpl_changed; s_tpl_changed = false; return v; }
int  net_canvas_epoch(void) { return s_epoch; }
int  net_canvas_count(void) { return s_ns; }
bool net_canvas_open(void)  { return s_open; }
const int16_t* net_path_points(int* n) { *n = s_n; return s_pts; }

const int16_t* net_canvas_stroke(int i, int* n)
{
  if (i < 0 || i >= s_ns) { *n = 0; return NULL; }
  *n = s_strokes[i].n;
  return s_strokes[i].xy;
}

static void strokes_clear_locked(void)
{
  for (int i = 0; i < s_ns; i++) free((void*)s_strokes[i].xy);
  s_ns = 0;
}

void net_canvas_forget(void)
{
  net_lock();
  strokes_clear_locked();
  s_srv_cn = 0;   // wait for the server's word on the next round
  net_unlock();
}

bool net_take_uploaded(void)  { bool v = s_ev_uploaded;  s_ev_uploaded = false;  return v; }
bool net_take_connected(void) { bool v = s_ev_connected; s_ev_connected = false; return v; }

void net_set_mode(net_mode_t m)
{
  if (m == s_mode) return;
  s_mode = m;
  s_mode_changed = true;
  if (s_task) xTaskNotifyGive(s_task);
}

// =============================================================================
// Path recording
// =============================================================================
static void new_id(void)
{
  s_seq++;
  nvs_handle_t h;
  if (nvs_open("sketch", NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u32(h, "seq", s_seq);
    nvs_commit(h);
    nvs_close(h);
  }
  snprintf(s_id, sizeof(s_id), "%s-%lu", s_badge, (unsigned long)s_seq);
}

void net_path_begin(int x, int y)
{
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  new_id();
  s_n = 0;
  s_live_from = 0;
  s_frozen = 0;
  if (s_pts) {
    s_pts[0] = (int16_t)x;
    s_pts[1] = (int16_t)y;
    s_n = 1;
  }
  xSemaphoreGive(s_mtx);
  if (s_task) xTaskNotifyGive(s_task);
}

void net_path_add(int x, int y)
{
  if (!s_pts) return;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  if (s_n >= 2 && s_n - 1 >= s_frozen) {
    // b -> c continues a -> b in the same direction: move b to c
    int ax = s_pts[2 * s_n - 4], ay = s_pts[2 * s_n - 3];
    int bx = s_pts[2 * s_n - 2], by = s_pts[2 * s_n - 1];
    int ux = bx - ax, uy = by - ay, vx = x - bx, vy = y - by;
    if (ux * vy - uy * vx == 0 && ux * vx + uy * vy > 0) {
      s_pts[2 * s_n - 2] = (int16_t)x;
      s_pts[2 * s_n - 1] = (int16_t)y;
      xSemaphoreGive(s_mtx);
      return;
    }
  }
  if (s_n < MAX_POINTS) {
    s_pts[2 * s_n] = (int16_t)x;
    s_pts[2 * s_n + 1] = (int16_t)y;
    s_n++;
  }
  xSemaphoreGive(s_mtx);
}

bool net_path_finish(void)
{
  if (!s_pts) return false;
  bool queued = false;
  xSemaphoreTake(s_mtx, portMAX_DELAY);
  int len = 0;
  for (int i = 1; i < s_n; i++) {
    len += abs(s_pts[2 * i] - s_pts[2 * i - 2]) + abs(s_pts[2 * i + 1] - s_pts[2 * i - 1]);
  }
  // Upload if the knobs drew something, or people drew on it from the website
  if (s_n >= 1 && (len >= MIN_DRAWING_PX || s_srv_wn > 0)) {
    drawing_t* d = calloc(1, sizeof(drawing_t));
    if (d) d->xy = ps_malloc((size_t)s_n * 4);
    if (d && d->xy) {
      strlcpy(d->id, s_id, sizeof(d->id));
      d->n = s_n;
      memcpy(d->xy, s_pts, (size_t)s_n * 4);
      if (xQueueSend(s_done_q, &d, 0) == pdTRUE) {
        queued = true;
        printf("net: drawing %s queued (%d points, %d px)\n", d->id, d->n, len);
      }
    }
    if (!queued && d) { free(d->xy); free(d); }
  }
  s_n = 0;
  xSemaphoreGive(s_mtx);
  if (s_task) xTaskNotifyGive(s_task);
  return queued;
}

// base64 -> points in PSRAM (caller frees), or NULL
static int16_t* b64_points(const char* p, int* n)
{
  *n = 0;
  if (!p) return NULL;
  size_t plen = strlen(p), cap = plen / 4 * 3 + 4, olen = 0;
  uint8_t* raw = ps_malloc(cap);
  if (!raw) return NULL;
  if (mbedtls_base64_decode(raw, cap, &olen, (const unsigned char*)p, plen) != 0 || olen < 4) {
    free(raw);
    return NULL;
  }
  *n = (int)(olen / 4);
  return (int16_t*)raw;
}

static int json_int(const cJSON* j, const char* k, int def)
{
  const cJSON* v = cJSON_GetObjectItem(j, k);
  return cJSON_IsNumber(v) ? v->valueint : def;
}

// =============================================================================
// HTTP
// =============================================================================
// One request. Returns the HTTP status (or -1 on a network error) and, if
// `out` is given, the body NUL-terminated in PSRAM (caller frees).
static int http(esp_http_client_method_t method, const char* path, const char* body, char** out)
{
  if (out) *out = NULL;
  char url[192];
  snprintf(url, sizeof(url), "%s%s", BADGE_SERVER, path);
  esp_http_client_config_t cfg = {
    .url = url,
    .method = method,
    .timeout_ms = HTTP_TIMEOUT_MS,
    .crt_bundle_attach = esp_crt_bundle_attach,
    .buffer_size = 4096,
    .buffer_size_tx = 1024,
  };
  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (!c) return -1;
  esp_http_client_set_header(c, "X-Badge-Key", BADGE_UPLOAD_KEY);
  int blen = body ? (int)strlen(body) : 0;
  if (body) esp_http_client_set_header(c, "Content-Type", "application/json");

  int status = -1;
  char* buf = NULL;
  if (esp_http_client_open(c, blen) != ESP_OK) goto done;
  if (blen && esp_http_client_write(c, body, blen) != blen) goto done;
  int64_t cl = esp_http_client_fetch_headers(c);
  status = esp_http_client_get_status_code(c);
  if (out) {
    size_t cap = (cl > 0 && cl < MAX_RESPONSE) ? (size_t)cl + 1 : 8192, len = 0;
    buf = ps_malloc(cap);
    while (buf) {
      if (len + 1 >= cap) {
        if (cap >= MAX_RESPONSE) break;
        size_t ncap = cap * 2;
        char* nb = ps_malloc(ncap);
        if (!nb) break;
        memcpy(nb, buf, len);
        free(buf);
        buf = nb;
        cap = ncap;
      }
      int r = esp_http_client_read(c, buf + len, (int)(cap - 1 - len));
      if (r <= 0) break;
      len += (size_t)r;
    }
    if (buf) { buf[len] = 0; *out = buf; buf = NULL; }
  }
done:
  esp_http_client_close(c);
  esp_http_client_cleanup(c);
  free(buf);
  return status;
}

// {"id":"..", ..., "p":"<base64 of n points>"} built in PSRAM. `extra` goes
// between the id and p, already JSON (",\"from\":3").
static char* drawing_json(const char* id, const char* extra, const int16_t* xy, int n)
{
  size_t raw = (size_t)n * 4, b64 = 4 * ((raw + 2) / 3) + 1;
  size_t cap = b64 + strlen(id) + strlen(extra) + 32;
  char* j = ps_malloc(cap);
  if (!j) return NULL;
  int k = snprintf(j, cap, "{\"id\":\"%s\"%s,\"p\":\"", id, extra);
  size_t olen = 0;
  // int16 pairs are already little-endian on the ESP32
  mbedtls_base64_encode((unsigned char*)j + k, cap - k, &olen, (const unsigned char*)xy, raw);
  snprintf(j + k + olen, cap - k - olen, "\"}");
  return j;
}

// =============================================================================
// Upload queue on flash: /spool/<seq>.bin = [id length][id][points]
// =============================================================================
static void spool_write(drawing_t* d)
{
  if (!s_spool_ok) return;
  const char* dash = strrchr(d->id, '-');
  char path[48];
  snprintf(path, sizeof(path), SPOOL_DIR "/%08lu.bin", strtoul(dash ? dash + 1 : "0", NULL, 10));
  FILE* f = fopen(path, "wb");
  if (!f) { printf("net: can't write %s\n", path); return; }
  uint8_t idlen = (uint8_t)strlen(d->id);
  bool ok = fwrite(&idlen, 1, 1, f) == 1 && fwrite(d->id, 1, idlen, f) == idlen &&
            fwrite(d->xy, 4, (size_t)d->n, f) == (size_t)d->n;
  fclose(f);
  if (!ok) { remove(path); printf("net: flash full? %s not saved\n", path); }
}

// Oldest file in the spool, or false if it's empty
static bool spool_oldest(char* path, size_t cap)
{
  if (!s_spool_ok) return false;
  DIR* dir = opendir(SPOOL_DIR);
  if (!dir) return false;
  char best[32] = "";
  struct dirent* e;
  while ((e = readdir(dir))) {
    if (!strstr(e->d_name, ".bin")) continue;
    if (!best[0] || strcmp(e->d_name, best) < 0) strlcpy(best, e->d_name, sizeof(best));
  }
  closedir(dir);
  if (!best[0]) return false;
  snprintf(path, cap, SPOOL_DIR "/%s", best);
  return true;
}

// Upload one queued drawing. Returns false to back off.
static bool spool_upload_one(const char* path)
{
  FILE* f = fopen(path, "rb");
  if (!f) return false;
  struct stat st;
  stat(path, &st);
  uint8_t idlen = 0;
  char id[32] = "";
  fread(&idlen, 1, 1, f);
  if (idlen >= sizeof(id)) idlen = sizeof(id) - 1;
  fread(id, 1, idlen, f);
  int n = (int)((st.st_size - 1 - idlen) / 4);
  int16_t* xy = n > 0 ? ps_malloc((size_t)n * 4) : NULL;
  bool read_ok = xy && fread(xy, 4, (size_t)n, f) == (size_t)n;
  fclose(f);
  if (!read_ok) {
    free(xy);
    printf("net: %s is unreadable, dropping it\n", path);
    remove(path);
    return true;
  }
  char* body = drawing_json(id, "", xy, n);
  free(xy);
  if (!body) return false;
  int status = http(HTTP_METHOD_POST, "/api/drawings", body, NULL);
  free(body);
  printf("net: upload %s (%d points) -> %d\n", id, n, status);
  if (status == 200) {
    remove(path);
    s_ev_uploaded = true;
    return true;
  }
  if (status == 400 || status == 413) { remove(path); return true; }   // never going to work
  if (status == 401) printf("net: the website refused BADGE_UPLOAD_KEY - check UPLOAD_KEY on Netlify\n");
  return false;
}

// =============================================================================
// Live view
// =============================================================================
static bool live_send(void)
{
  char id[32];
  int from, count;
  int16_t* chunk = NULL;

  xSemaphoreTake(s_mtx, portMAX_DELAY);
  strlcpy(id, s_id, sizeof(id));
  from = s_live_from;
  count = s_n - from;
  if (count < 0) { from = 0; count = s_n; }
  if (count > LIVE_CHUNK) count = LIVE_CHUNK;
  if (count > 0 && (chunk = ps_malloc((size_t)count * 4))) {
    memcpy(chunk, s_pts + 2 * from, (size_t)count * 4);
    if (from + count > s_frozen) s_frozen = from + count;
  } else {
    count = 0;
  }
  xSemaphoreGive(s_mtx);

  char extra[64];
  snprintf(extra, sizeof(extra), ",\"from\":%d,\"mode\":\"%s\"", from,
           s_mode == NET_MODE_GALLERY ? "gallery" : "drawing");
  char* body = drawing_json(id, extra, chunk, count);
  free(chunk);
  if (!body) return false;
  char* resp = NULL;
  int status = http(HTTP_METHOD_POST, "/api/live", body, &resp);
  free(body);
  bool ok = status == 200;
  if (ok && resp) {
    cJSON* j = cJSON_Parse(resp);
    const cJSON* need = cJSON_GetObjectItem(j, "need");
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (strcmp(id, s_id) == 0) {   // still the same drawing
      if (cJSON_IsNumber(need)) s_live_from = need->valueint <= s_n ? need->valueint : 0;
      else s_live_from = from + count;
    }
    xSemaphoreGive(s_mtx);
    if (cJSON_GetObjectItem(j, "cv")) {
      s_srv_cv = json_int(j, "cv", s_srv_cv);
      s_srv_cn = json_int(j, "cn", s_srv_cn);
      s_srv_wn = json_int(j, "wn", s_srv_wn);
      s_srv_tv = json_int(j, "tv", s_srv_tv);
      s_open = cJSON_IsTrue(cJSON_GetObjectItem(j, "open"));
    }
    cJSON_Delete(j);
  }
  free(resp);
  return ok;
}

// =============================================================================
// Gallery
// =============================================================================
void net_gallery_free(void)
{
  if (s_gal_state == NET_GALLERY_LOADING) return;   // the task owns it right now
  for (int i = 0; i < s_gal_n; i++) {
    free((void*)s_gal[i].xy);
    for (int k = 0; k < s_gal[i].ns; k++) free((void*)s_gal[i].s[k].xy);
    free(s_gal[i].s);
  }
  memset(s_gal, 0, sizeof(s_gal));
  s_gal_n = 0;
  s_gal_state = NET_GALLERY_IDLE;
}

void net_gallery_request(void)
{
  if (s_gal_state == NET_GALLERY_LOADING) return;
  net_gallery_free();
  if (!net_configured()) { s_gal_state = NET_GALLERY_FAILED; return; }
  s_gal_state = NET_GALLERY_LOADING;
  s_gal_req = true;
  if (s_task) xTaskNotifyGive(s_task);
}

net_gallery_state_t net_gallery_state(void) { return s_gal_state; }
int net_gallery_count(void) { return s_gal_state == NET_GALLERY_READY ? s_gal_n : 0; }
const net_gallery_item_t* net_gallery_item(int i)
{
  return (s_gal_state == NET_GALLERY_READY && i >= 0 && i < s_gal_n) ? &s_gal[i] : NULL;
}

static void gallery_fetch(void)
{
  char* resp = NULL;
  char path[64];
  snprintf(path, sizeof(path), "/api/drawings?format=badge&limit=%d", GALLERY_LIMIT);
  int status = http(HTTP_METHOD_GET, path, NULL, &resp);
  cJSON* j = (status == 200 && resp) ? cJSON_Parse(resp) : NULL;
  free(resp);
  const cJSON* items = cJSON_GetObjectItem(j, "items");
  int n = 0;
  const cJSON* it;
  cJSON_ArrayForEach(it, items) {
    if (n >= GALLERY_LIMIT) break;
    const char* t = cJSON_GetStringValue(cJSON_GetObjectItem(it, "t"));
    int np = 0;
    int16_t* xy = b64_points(cJSON_GetStringValue(cJSON_GetObjectItem(it, "p")), &np);
    if (!xy) continue;
    net_gallery_item_t* g = &s_gal[n];
    strlcpy(g->t, t ? t : "", sizeof(g->t));
    g->n = np;
    g->xy = xy;
    const cJSON* ss = cJSON_GetObjectItem(it, "s");
    int cnt = cJSON_GetArraySize(ss);
    g->s = cnt > 0 ? calloc((size_t)cnt, sizeof(net_stroke_t)) : NULL;
    g->ns = 0;
    const cJSON* e;
    cJSON_ArrayForEach(e, ss) {
      if (!g->s) break;
      int ns = 0;
      int16_t* sxy = b64_points(cJSON_GetStringValue(e), &ns);
      if (sxy) { g->s[g->ns].xy = sxy; g->s[g->ns].n = ns; g->ns++; }
    }
    n++;
  }
  cJSON_Delete(j);
  s_gal_n = n;
  printf("net: gallery -> %d, %d drawings\n", status, n);
  s_gal_state = (status == 200) ? NET_GALLERY_READY : NET_GALLERY_FAILED;
}

// =============================================================================
// Open canvas and template
// =============================================================================
static void canvas_fetch(void)
{
  int cv, from;
  net_lock();
  cv = s_cv;
  from = s_ns;
  net_unlock();
  char path[64], *resp = NULL;
  snprintf(path, sizeof(path), "/api/canvas?cv=%d&from=%d", cv, from);
  int status = http(HTTP_METHOD_GET, path, NULL, &resp);
  cJSON* j = (status == 200 && resp) ? cJSON_Parse(resp) : NULL;
  free(resp);
  if (!j) { printf("net: canvas -> %d\n", status); return; }

  // Decode before taking the lock; the app may be drawing from the list
  const cJSON* arr = cJSON_GetObjectItem(j, "s");
  int cnt = cJSON_GetArraySize(arr), got = 0;
  net_stroke_t* add = cnt ? calloc((size_t)cnt, sizeof(net_stroke_t)) : NULL;
  const cJSON* e;
  cJSON_ArrayForEach(e, arr) {
    if (!add) break;
    int n = 0;
    int16_t* xy = b64_points(cJSON_GetStringValue(e), &n);
    if (xy) { add[got].xy = xy; add[got].n = n; got++; }
  }
  int rcv = json_int(j, "cv", cv), rfrom = json_int(j, "from", 0);
  cJSON_Delete(j);

  net_lock();
  if (rcv != s_cv) {   // a new round started: what we hold is gone from the screen
    strokes_clear_locked();
    s_cv = rcv;
    s_epoch++;
  }
  if (rfrom == s_ns && got) {
    if (s_ns + got > s_ns_cap) {
      int cap = (s_ns + got) * 2;
      net_stroke_t* ns = heap_caps_realloc(s_strokes, (size_t)cap * sizeof(net_stroke_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (ns) { s_strokes = ns; s_ns_cap = cap; }
    }
    if (s_ns + got <= s_ns_cap) {
      memcpy(&s_strokes[s_ns], add, (size_t)got * sizeof(net_stroke_t));
      s_ns += got;
      got = 0;   // handed over
    }
  }
  net_unlock();
  for (int i = 0; i < got; i++) free((void*)add[i].xy);   // out of step: fetch again next time
  free(add);
  printf("net: canvas round %d, %d lines\n", rcv, s_ns);
}

static void template_fetch(void)
{
  char* resp = NULL;
  int status = http(HTTP_METHOD_GET, "/api/canvas?part=template", NULL, &resp);
  cJSON* j = (status == 200 && resp) ? cJSON_Parse(resp) : NULL;
  free(resp);
  if (!j) { printf("net: template -> %d\n", status); return; }
  int tv = json_int(j, "tv", 0);
  const char* m = cJSON_GetStringValue(cJSON_GetObjectItem(j, "m"));
  uint8_t* mask = NULL;
  if (m && m[0]) {
    size_t olen = 0;
    mask = ps_malloc(TPL_BYTES + 4);
    if (mask && (mbedtls_base64_decode(mask, TPL_BYTES + 4, &olen, (const unsigned char*)m, strlen(m)) != 0 ||
                 olen != TPL_BYTES)) {
      free(mask);
      mask = NULL;
      printf("net: template is the wrong size\n");
    }
  }
  printf("net: template %d: %s\n", tv, mask ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "name")) : "none");
  cJSON_Delete(j);
  net_lock();
  uint8_t* old = s_tpl;
  s_tpl = mask;
  s_tv = tv;
  s_tpl_changed = true;
  net_unlock();
  free(old);
}

// =============================================================================
// Wi-Fi
// =============================================================================
static esp_timer_handle_t s_reconnect;

static void reconnect_cb(void* arg) { (void)arg; esp_wifi_connect(); }

static void wifi_event(void* arg, esp_event_base_t base, int32_t id, void* data)
{
  (void)arg;
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t* e = data;
    if (s_online) printf("net: Wi-Fi lost (reason %d)\n", e->reason);
    else          printf("net: can't join \"%s\" (reason %d), retrying\n", WIFI_SSID, e->reason);
    s_online = false;
    xEventGroupClearBits(s_ev, EV_ONLINE);
    esp_timer_stop(s_reconnect);
    esp_timer_start_once(s_reconnect, 3000 * 1000);
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t* e = data;
    printf("net: on \"%s\" as " IPSTR "\n", WIFI_SSID, IP2STR(&e->ip_info.ip));
    s_online = true;
    s_ev_connected = true;
    xEventGroupSetBits(s_ev, EV_ONLINE);
    if (s_task) xTaskNotifyGive(s_task);
  }
}

static void wifi_start(void)
{
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&ic));
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
  esp_timer_create_args_t ta = { .callback = reconnect_cb, .name = "wifi_retry" };
  esp_timer_create(&ta, &s_reconnect);

  wifi_config_t wc = { 0 };
  strlcpy((char*)wc.sta.ssid, WIFI_SSID, sizeof(wc.sta.ssid));
  strlcpy((char*)wc.sta.password, WIFI_PASS, sizeof(wc.sta.password));
  wc.sta.threshold.authmode = WIFI_PASS[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
  wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;          // a venue has many APs with one name:
  wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;      // take the strongest
  wc.sta.pmf_cfg.capable = true;
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_set_config(WIFI_IF_STA, &wc);
  esp_wifi_start();
}

// =============================================================================
// Task
// =============================================================================
static void net_task(void* arg)
{
  (void)arg;
  uint32_t retry_ms = RETRY_MIN_MS, next_upload = 0, last_live = 0;
  char last_live_id[32] = "";
  char path[48];

  for (;;) {
    drawing_t* d;
    while (xQueueReceive(s_done_q, &d, 0) == pdTRUE) {
      spool_write(d);
      free(d->xy);
      free(d);
      next_upload = 0;
    }

    if (!s_online || !net_configured()) {
      if (s_gal_req) { s_gal_req = false; s_gal_state = NET_GALLERY_FAILED; }
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
      continue;
    }
    uint32_t t = now_ms();

    if (s_gal_req) {
      s_gal_req = false;
      gallery_fetch();
      continue;
    }

    if ((int32_t)(t - next_upload) >= 0 && spool_oldest(path, sizeof(path))) {
      if (spool_upload_one(path)) {
        retry_ms = RETRY_MIN_MS;
        next_upload = 0;
        continue;
      }
      next_upload = t + retry_ms;
      if ((retry_ms *= 2) > RETRY_MAX_MS) retry_ms = RETRY_MAX_MS;
    }

    // Live: new points every LIVE_MS, a heartbeat every HEARTBEAT_MS, and right
    // away when the mode or the drawing changes.
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    bool fresh = s_n != s_live_from;
    bool new_drawing = strcmp(last_live_id, s_id) != 0;
    xSemaphoreGive(s_mtx);
    bool due = s_mode_changed || new_drawing || (fresh && t - last_live >= LIVE_MS) ||
               (s_open && t - last_live >= LIVE_MS) ||   // the canvas is open: check often
               t - last_live >= HEARTBEAT_MS;
    if (due) {
      s_mode_changed = false;
      xSemaphoreTake(s_mtx, portMAX_DELAY);
      strlcpy(last_live_id, s_id, sizeof(last_live_id));
      xSemaphoreGive(s_mtx);
      if (!live_send()) printf("net: live update failed\n");
      last_live = now_ms();
    }

    // New lines from the website, or a new template?
    if (s_srv_cv && s_srv_cv != s_cv) {
      net_lock();
      if (s_srv_cv != s_cv) { strokes_clear_locked(); s_cv = s_srv_cv; s_epoch++; }
      net_unlock();
    }
    if (s_srv_cn > s_ns) canvas_fetch();
    if (s_srv_tv != s_tv) template_fetch();

    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250));
  }
}

void net_start(void)
{
  s_mtx = xSemaphoreCreateMutex();
  s_ev = xEventGroupCreate();
  s_done_q = xQueueCreate(8, sizeof(drawing_t*));
  s_pts = heap_caps_malloc((size_t)MAX_POINTS * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_pts) printf("net: no PSRAM for the path, drawings won't be saved\n");

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  nvs_handle_t h;
  if (nvs_open("sketch", NVS_READONLY, &h) == ESP_OK) {
    nvs_get_u32(h, "seq", &s_seq);
    nvs_close(h);
  }
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_badge, sizeof(s_badge), "%02x%02x%02x", mac[3], mac[4], mac[5]);

  esp_vfs_spiffs_conf_t sc = {
    .base_path = SPOOL_DIR, .partition_label = "storage", .max_files = 4, .format_if_mount_failed = true,
  };
  s_spool_ok = esp_vfs_spiffs_register(&sc) == ESP_OK;
  if (!s_spool_ok) printf("net: no storage partition, drawings made offline won't survive a reboot\n");

  if (!WIFI_SSID[0]) {
    printf("net: no Wi-Fi set in wifi_secrets.h\n");
  } else {
    if (!net_configured()) printf("net: BADGE_SERVER isn't set, drawings will wait on flash\n");
    wifi_start();
  }
  xTaskCreatePinnedToCore(net_task, "net", 8192, NULL, 3, &s_task, 0);
}

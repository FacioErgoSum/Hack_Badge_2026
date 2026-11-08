// -----------------------------------------------------------------------------
// net - Wi-Fi, drawing uploads, the live view and the gallery download.
//
// Everything network-side runs in its own task on core 0, so the knobs and the
// panel never wait on it. The app only talks to it through these calls.
//
// A drawing is the pen's path: points in frame coordinates. The app reports
// every pen move with net_path_add(); runs of moves in one straight line are
// merged into a single point, so turning one knob costs almost nothing.
// net_path_finish() closes the drawing and queues it for upload. The queue is
// a folder on flash, so drawings made offline or before a reboot go up later.
// -----------------------------------------------------------------------------
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NET_MODE_DRAWING = 0, NET_MODE_GALLERY = 1 } net_mode_t;

typedef enum {
  NET_GALLERY_IDLE = 0,
  NET_GALLERY_LOADING,
  NET_GALLERY_READY,
  NET_GALLERY_FAILED
} net_gallery_state_t;

typedef struct {
  int n;               // points
  const int16_t* xy;   // x0, y0, x1, y1, ...
} net_stroke_t;

typedef struct {
  char t[24];          // "Nov 8 2:14 PM", from the server
  int n;               // points
  const int16_t* xy;   // x0, y0, x1, y1, ...
  int ns;              // lines drawn on it from the website
  net_stroke_t* s;
} net_gallery_item_t;

// The tracing template: a 1-bit mask over this area of the frame, MSB first,
// TPL_W / 8 bytes a row. Shown dithered (every other pixel) so it reads grey.
#define NET_TPL_X0 128
#define NET_TPL_Y0 116
#define NET_TPL_W  704
#define NET_TPL_H  412

// Bring up NVS, the upload queue on flash, Wi-Fi and the network task.
void net_start(void);

bool net_online(void);         // associated and holding an IP
bool net_configured(void);     // a website URL is set in wifi_secrets.h

// The drawing in progress
void net_path_begin(int x, int y);   // start a new drawing with the pen here
void net_path_add(int x, int y);     // the pen moved here
// Close the drawing and queue it for upload if it's more than a scribble.
// Returns true if it was queued. Call net_path_begin() for the next one.
bool net_path_finish(void);

void net_set_mode(net_mode_t m);

// Gallery: ask for the newest drawings, poll until READY or FAILED.
void net_gallery_request(void);
net_gallery_state_t net_gallery_state(void);
int  net_gallery_count(void);
const net_gallery_item_t* net_gallery_item(int i);
void net_gallery_free(void);

// Template and open canvas. Hold net_lock() while using the pointers these
// return; the network task swaps them under the same lock.
void net_lock(void);
void net_unlock(void);
const uint8_t* net_template(void);          // NULL: no template
bool net_take_template_changed(void);       // once per new template (or removal)
int  net_canvas_epoch(void);                // goes up when the canvas lines start over
int  net_canvas_count(void);                // lines held right now
const int16_t* net_canvas_stroke(int i, int* n);
bool net_canvas_open(void);
void net_canvas_forget(void);               // the screen was erased: drop the lines held
const int16_t* net_path_points(int* n);     // the drawing in progress (under the lock)

// One-shot events for the LEDs; each returns true once per occurrence.
bool net_take_uploaded(void);
bool net_take_connected(void);

#ifdef __cplusplus
}
#endif

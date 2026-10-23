// Fixed evdev keycodes on a private compositor socket. The Python harness
// checks both levels of owned window focus before writing each input line.
#define _GNU_SOURCE
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "tipe-virtual-keyboard-client.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static struct zwp_virtual_keyboard_manager_v1 *manager;
static struct wl_seat *seat;
static void global(void *data, struct wl_registry *registry, uint32_t name,
                   const char *interface, uint32_t version) {
  (void)data; (void)version;
  if (!strcmp(interface, "zwp_virtual_keyboard_manager_v1"))
    manager = wl_registry_bind(registry, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
  if (!strcmp(interface, "wl_seat"))
    seat = wl_registry_bind(registry, name, &wl_seat_interface, 1);
}
static void removed(void *data, struct wl_registry *registry, uint32_t name) {
  (void)data; (void)registry; (void)name;
}
int main(void) {
  struct wl_display *display = wl_display_connect(NULL);
  if (!display) return 2;
  struct wl_registry *registry = wl_display_get_registry(display);
  const struct wl_registry_listener listener = {global, removed};
  wl_registry_add_listener(registry, &listener, NULL);
  if (wl_display_roundtrip(display) < 0 || !manager || !seat) return 3;
  struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  struct xkb_rule_names names = {.rules="evdev", .model="pc105", .layout="us"};
  struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
  if (!keymap) return 4;
  char *text = xkb_keymap_get_as_string(keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
  size_t length = strlen(text)+1;
  int fd = memfd_create("tipe-private-keymap", MFD_CLOEXEC);
  if (fd < 0 || write(fd, text, length) != (ssize_t)length) return 5;
  struct zwp_virtual_keyboard_v1 *keyboard =
      zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(manager, seat);
  zwp_virtual_keyboard_v1_keymap(keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, length);
  if (wl_display_roundtrip(display) < 0) return 6;
  close(fd); free(text); xkb_keymap_unref(keymap); xkb_context_unref(context);
  puts("ready"); fflush(stdout);
  unsigned code, state;
  while (scanf("%u %u", &code, &state) == 2) {
    if (code > 767 || state > 1) return 7;
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    uint32_t ms = (uint32_t)(now.tv_sec*1000+now.tv_nsec/1000000);
    zwp_virtual_keyboard_v1_key(keyboard, ms, code, state);
    if (wl_display_roundtrip(display) < 0) return 8;
    puts("ok"); fflush(stdout);
  }
  zwp_virtual_keyboard_v1_destroy(keyboard);
  zwp_virtual_keyboard_manager_v1_destroy(manager);
  wl_seat_destroy(seat); wl_registry_destroy(registry);
  wl_display_flush(display); wl_display_disconnect(display);
  return 0;
}

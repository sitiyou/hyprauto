#define _GNU_SOURCE
#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>
#include "xdg-shell-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"
#include "linux-dmabuf-v1-client-protocol.h"
#include <gbm.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct wl_display*    display;
static struct wl_compositor* compositor;
static struct wl_shm*        shm;
static struct wl_seat*       seat;
static struct xdg_wm_base*   shell;
static struct wl_surface*    surface;
static struct xdg_surface*   xsurface;
static struct xkb_context*   context;
static struct xkb_keymap*    keymap;
static struct xkb_state*     state;
static int                   width = 320, height = 240, running = 1;
static struct wp_cursor_shape_manager_v1* shape_manager;
static struct wl_surface*                 other_surface;
static struct xdg_surface*                other_xsurface;
static struct xdg_toplevel*               other_top;
static struct wl_keyboard*                keyboards[2];
static struct wl_surface*                 keyboard_surfaces[2];
static struct wl_pointer*                 pointers[2];
static struct wl_surface*                 pointer_surfaces[2];
static uint32_t                           cursor_serials[2];
static struct wp_cursor_shape_device_v1*  shapes[2];
static int                                input_count;
static struct zwp_linux_dmabuf_v1*        dmabuf;
static struct gbm_device*                 gbm;
static unsigned                           buffer_id;

struct dma_buffer {
    struct gbm_bo* bo;
    unsigned       id;
};

static void dma_buffer_release(void* data, struct wl_buffer* buffer) {
    struct dma_buffer* allocation = data;
    printf("buffer-release %u\n", allocation->id);
    wl_buffer_destroy(buffer);
    gbm_bo_destroy(allocation->bo);
    free(allocation);
}
static const struct wl_buffer_listener dma_buffer_listener = {dma_buffer_release};

static void                  buffer_release(void* data, struct wl_buffer* buffer) {
    wl_buffer_destroy(buffer);
}
static const struct wl_buffer_listener buffer_listener = {buffer_release};

static void                            fill_pixels(uint32_t* pixels, size_t stride) {
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[(size_t)y * stride + x] = y >= height - 32 && x >= width - 32 ? 0xff204080 : y < 32 && x < 32 ? 0x80402010 : 0xff305070;
}

static void submit_buffer(struct wl_surface* configured) {
    if (gbm) {
        struct dma_buffer* allocation = calloc(1, sizeof(*allocation));
        if (!allocation)
            exit(2);
        allocation->bo = gbm_bo_create(gbm, width, height, GBM_FORMAT_ARGB8888, GBM_BO_USE_RENDERING | GBM_BO_USE_LINEAR);
        if (!allocation->bo || gbm_bo_get_plane_count(allocation->bo) != 1) {
            perror("gbm_bo_create");
            exit(2);
        }
        uint32_t  stride;
        void*     mapping = NULL;
        uint32_t* pixels  = gbm_bo_map(allocation->bo, 0, 0, width, height, GBM_BO_TRANSFER_WRITE, &stride, &mapping);
        if (!pixels) {
            perror("gbm_bo_map");
            exit(2);
        }
        fill_pixels(pixels, stride / 4);
        gbm_bo_unmap(allocation->bo, mapping);
        int fd = gbm_bo_get_fd(allocation->bo);
        if (fd < 0) {
            perror("gbm_bo_get_fd");
            exit(2);
        }
        uint64_t                           modifier = gbm_bo_get_modifier(allocation->bo);
        struct zwp_linux_buffer_params_v1* params   = zwp_linux_dmabuf_v1_create_params(dmabuf);
        zwp_linux_buffer_params_v1_add(params, fd, 0, gbm_bo_get_offset(allocation->bo, 0), gbm_bo_get_stride(allocation->bo), modifier >> 32, modifier);
        struct wl_buffer* buffer = zwp_linux_buffer_params_v1_create_immed(params, width, height, GBM_FORMAT_ARGB8888, 0);
        allocation->id           = ++buffer_id;
        wl_buffer_add_listener(buffer, &dma_buffer_listener, allocation);
        zwp_linux_buffer_params_v1_destroy(params);
        close(fd);
        wl_surface_attach(configured, buffer, 0, 0);
        wl_surface_damage_buffer(configured, 0, 0, width, height);
        wl_surface_commit(configured);
        printf("buffer-submitted %u\n", allocation->id);
        return;
    }
    size_t size = (size_t)width * height * 4;
    int    fd   = memfd_create("hyprauto-test", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size) != 0)
        exit(2);
    uint32_t* pixels = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED)
        exit(2);
    fill_pixels(pixels, width);
    struct wl_shm_pool* pool   = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer*   buffer = wl_shm_pool_create_buffer(pool, 0, width, height, width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(buffer, &buffer_listener, NULL);
    wl_shm_pool_destroy(pool);
    munmap(pixels, size);
    close(fd);
    wl_surface_attach(configured, buffer, 0, 0);
    wl_surface_damage_buffer(configured, 0, 0, width, height);
    wl_surface_commit(configured);
}

static void configure_surface(void* data, struct xdg_surface* xdg, uint32_t serial) {
    xdg_surface_ack_configure(xdg, serial);
    submit_buffer(xdg == other_xsurface ? other_surface : surface);
    printf("configured %d %d\n", width, height);
}
static const struct xdg_surface_listener surface_listener = {configure_surface};

static void                              configure_toplevel(void* data, struct xdg_toplevel* top, int32_t w, int32_t h, struct wl_array* states) {
    if (w > 0)
        width = w;
    if (h > 0)
        height = h;
    int       activated = 0;
    uint32_t* value;
    wl_array_for_each(value, states) if (*value == XDG_TOPLEVEL_STATE_ACTIVATED) activated = 1;
    printf("active %d\n", activated);
}
static void close_toplevel(void* data, struct xdg_toplevel* top) {
    running = 0;
}
static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = configure_toplevel,
    .close     = close_toplevel,
};

static void ping(void* data, struct xdg_wm_base* base, uint32_t serial) {
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener shell_listener = {ping};

static void                              keyboard_keymap(void* data, struct wl_keyboard* keyboard, uint32_t format, int32_t fd, uint32_t size) {
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1)
        exit(2);
    char* map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED)
        exit(2);
    if (state)
        xkb_state_unref(state);
    if (keymap)
        xkb_keymap_unref(keymap);
    keymap = xkb_keymap_new_from_string(context, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    close(fd);
    if (!keymap || !(state = xkb_state_new(keymap)))
        exit(2);
    puts("keymap");
}
static void keyboard_enter(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surf, struct wl_array* keys) {
    for (int i = 0; i < input_count; ++i)
        if (keyboards[i] == keyboard)
            keyboard_surfaces[i] = surf;
    printf("keyboard-enter%s", surf == other_surface ? "-other" : "");
    uint32_t* key;
    wl_array_for_each(key, keys) printf(" %u", *key);
    puts("");
}
static void keyboard_leave(void* data, struct wl_keyboard* keyboard, uint32_t serial, struct wl_surface* surf) {
    puts("keyboard-leave");
}
static void keyboard_key(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t time, uint32_t key, uint32_t pressed) {
    int other = 0;
    for (int i = 0; i < input_count; ++i)
        if (keyboards[i] == keyboard)
            other = keyboard_surfaces[i] == other_surface;
    printf("%skey %u %u %u\n", other ? "other-" : "", key, pressed, state ? xkb_state_key_get_one_sym(state, key + 8) : 0);
}
static void keyboard_modifiers(void* data, struct wl_keyboard* keyboard, uint32_t serial, uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group) {
    if (state)
        xkb_state_update_mask(state, depressed, latched, locked, 0, 0, group);
    printf("modifiers %u %u %u %u\n", depressed, latched, locked, group);
}
static void                              keyboard_repeat(void* data, struct wl_keyboard* keyboard, int32_t rate, int32_t delay) {}
static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_keymap, keyboard_enter, keyboard_leave, keyboard_key, keyboard_modifiers, keyboard_repeat,
};

static void pointer_enter(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surf, wl_fixed_t x, wl_fixed_t y) {
    for (int i = 0; i < input_count; ++i)
        if (pointers[i] == pointer) {
            cursor_serials[i]   = serial;
            pointer_surfaces[i] = surf;
        }
    printf("pointer-enter%s %.3f %.3f\n", surf == other_surface ? "-other" : "", wl_fixed_to_double(x), wl_fixed_to_double(y));
    wl_pointer_set_cursor(pointer, serial, NULL, 0, 0);
}
static void pointer_leave(void* data, struct wl_pointer* pointer, uint32_t serial, struct wl_surface* surf) {
    puts("pointer-leave");
}
static void pointer_motion(void* data, struct wl_pointer* pointer, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    int other = 0;
    for (int i = 0; i < input_count; ++i)
        if (pointers[i] == pointer)
            other = pointer_surfaces[i] == other_surface;
    printf("%smotion %.3f %.3f\n", other ? "other-" : "", wl_fixed_to_double(x), wl_fixed_to_double(y));
}
static void pointer_button(void* data, struct wl_pointer* pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t pressed) {
    int other = 0;
    for (int i = 0; i < input_count; ++i)
        if (pointers[i] == pointer)
            other = pointer_surfaces[i] == other_surface;
    printf("%sbutton %u %u\n", other ? "other-" : "", button, pressed);
}
static void pointer_axis(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {
    printf("axis %u %.3f\n", axis, wl_fixed_to_double(value));
}
static void pointer_frame(void* data, struct wl_pointer* pointer) {
    puts("frame");
}
static void                             axis_source(void* data, struct wl_pointer* pointer, uint32_t source) {}
static void                             axis_stop(void* data, struct wl_pointer* pointer, uint32_t time, uint32_t axis) {}
static void                             axis_discrete(void* data, struct wl_pointer* pointer, uint32_t axis, int32_t value) {}
static void                             axis_value120(void* data, struct wl_pointer* pointer, uint32_t axis, int32_t value) {}
static void                             axis_direction(void* data, struct wl_pointer* pointer, uint32_t axis, uint32_t direction) {}
static const struct wl_pointer_listener pointer_listener = {
    .enter                   = pointer_enter,
    .leave                   = pointer_leave,
    .motion                  = pointer_motion,
    .button                  = pointer_button,
    .axis                    = pointer_axis,
    .frame                   = pointer_frame,
    .axis_source             = axis_source,
    .axis_stop               = axis_stop,
    .axis_discrete           = axis_discrete,
    .axis_value120           = axis_value120,
    .axis_relative_direction = axis_direction,
};
static void                          seat_capabilities(void* data, struct wl_seat* s, uint32_t caps) {}
static void                          seat_name(void* data, struct wl_seat* s, const char* name) {}
static const struct wl_seat_listener seat_listener = {seat_capabilities, seat_name};

static void                                      dma_format(void* data, struct zwp_linux_dmabuf_v1* resource, uint32_t format) {}
static void                                      dma_modifier(void* data, struct zwp_linux_dmabuf_v1* resource, uint32_t format, uint32_t high, uint32_t low) {}
static const struct zwp_linux_dmabuf_v1_listener dma_listener = {dma_format, dma_modifier};

static void                          global(void* data, struct wl_registry* registry, uint32_t name, const char* interface, uint32_t version) {
    if (!strcmp(interface, "wl_compositor"))
        compositor = wl_registry_bind(registry, name, &wl_compositor_interface, 4);
    else if (!strcmp(interface, "wl_shm"))
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, "zwp_linux_dmabuf_v1") && version >= 3) {
        dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface, 3);
        zwp_linux_dmabuf_v1_add_listener(dmabuf, &dma_listener, NULL);
    } else if (!strcmp(interface, "wl_seat")) {
        seat = wl_registry_bind(registry, name, &wl_seat_interface, version < 9 ? version : 9);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    } else if (!strcmp(interface, "wp_cursor_shape_manager_v1")) {
        shape_manager = wl_registry_bind(registry, name, &wp_cursor_shape_manager_v1_interface, 1);
    } else if (!strcmp(interface, "xdg_wm_base")) {
        shell = wl_registry_bind(registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(shell, &shell_listener, NULL);
    }
}
static void                              global_remove(void* data, struct wl_registry* registry, uint32_t name) {}
static const struct wl_registry_listener registry_listener = {global, global_remove};

static void                              bind_input(void) {
    int i        = input_count++;
    keyboards[i] = wl_seat_get_keyboard(seat);
    wl_keyboard_add_listener(keyboards[i], &keyboard_listener, NULL);
    pointers[i] = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(pointers[i], &pointer_listener, NULL);
    if (shape_manager)
        shapes[i] = wp_cursor_shape_manager_v1_get_pointer(shape_manager, pointers[i]);
}

int                                      main(int argc, char** argv) {
    if (argc != 2)
        return 2;
    setbuf(stdout, NULL);
    context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    display = wl_display_connect(NULL);
    if (!display || !context)
        return 2;
    struct wl_registry* registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    if (wl_display_roundtrip(display) < 0 || !compositor || !shm || !seat || !shell)
        return 2;
    int render_fd = -1;
    if (getenv("HYPRAUTO_TEST_DMABUF")) {
        const char* node = getenv("AUTO_INPUT_RENDER_NODE");
        if (!dmabuf || !node || (render_fd = open(node, O_RDWR | O_CLOEXEC)) < 0 || !(gbm = gbm_create_device(render_fd)))
            return 2;
    }
    bind_input();
    surface  = wl_compositor_create_surface(compositor);
    xsurface = xdg_wm_base_get_xdg_surface(shell, surface);
    xdg_surface_add_listener(xsurface, &surface_listener, NULL);
    struct xdg_toplevel* top = xdg_surface_get_toplevel(xsurface);
    xdg_toplevel_add_listener(top, &toplevel_listener, NULL);
    xdg_toplevel_set_app_id(top, argv[1]);
    xdg_toplevel_set_title(top, argv[1]);
    wl_surface_commit(surface);
    while (running) {
        while (wl_display_prepare_read(display) != 0)
            if (wl_display_dispatch_pending(display) < 0)
                goto done;
        if (wl_display_flush(display) < 0) {
            wl_display_cancel_read(display);
            break;
        }
        struct pollfd fds[] = {{wl_display_get_fd(display), POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}};
        if (poll(fds, 2, -1) < 0) {
            wl_display_cancel_read(display);
            break;
        }
        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(display) < 0)
                break;
            if (wl_display_dispatch_pending(display) < 0)
                break;
        } else
            wl_display_cancel_read(display);
        if (fds[0].revents & (POLLERR | POLLHUP))
            break;
        if (fds[1].revents & POLLIN) {
            char request[64];
            if (!fgets(request, sizeof(request), stdin))
                break;
            if (!strncmp(request, "sync", 4)) {
                if (wl_display_roundtrip(display) < 0)
                    break;
                puts("sync");
            } else if (!strncmp(request, "redraw", 6)) {
                submit_buffer(surface);
                if (wl_display_roundtrip(display) < 0)
                    break;
                puts("redrawn");
            } else if (!strncmp(request, "unmap", 5)) {
                wl_surface_attach(surface, NULL, 0, 0);
                wl_surface_commit(surface);
                if (wl_display_roundtrip(display) < 0)
                    break;
                puts("unmapped");
            } else if (!strncmp(request, "cross-shape", 11) && input_count == 2) {
                wp_cursor_shape_device_v1_set_shape(shapes[1], cursor_serials[0], WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
                if (wl_display_roundtrip(display) < 0)
                    break;
                puts("cross-shape-set");
            } else if (!strncmp(request, "shape", 5)) {
                if (!shapes[0])
                    exit(2);
                for (int i = 0; i < input_count; ++i)
                    wp_cursor_shape_device_v1_set_shape(shapes[i], cursor_serials[i], WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
                if (wl_display_roundtrip(display) < 0)
                    break;
                puts("shape-set");
            } else if (!strncmp(request, "stale-cursor", 12)) {
                wl_pointer_set_cursor(pointers[0], cursor_serials[0] - 1, NULL, 0, 0);
                wp_cursor_shape_device_v1_set_shape(shapes[0], cursor_serials[0] - 1, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT);
                if (wl_display_roundtrip(display) < 0)
                    break;
                puts("stale-cursor-set");
            } else if (!strncmp(request, "cursor", 6)) {
                for (int i = 0; i < input_count; ++i)
                    wl_pointer_set_cursor(pointers[i], cursor_serials[i], NULL, 0, 0);
                if (wl_display_roundtrip(display) < 0)
                    break;
                puts("cursor-set");
            } else if (!strncmp(request, "bind", 4) && input_count == 1) {
                bind_input();
                if (wl_display_roundtrip(display) < 0)
                    break;
                puts("bound");
            } else if (!strncmp(request, "unbind", 6) && input_count == 2) {
                wp_cursor_shape_device_v1_destroy(shapes[1]);
                wl_pointer_release(pointers[1]);
                wl_keyboard_release(keyboards[1]);
                input_count = 1;
                if (wl_display_roundtrip(display) < 0)
                    break;
                puts("unbound");
            } else if (!strncmp(request, "window", 6) && !other_surface) {
                other_surface  = wl_compositor_create_surface(compositor);
                other_xsurface = xdg_wm_base_get_xdg_surface(shell, other_surface);
                xdg_surface_add_listener(other_xsurface, &surface_listener, NULL);
                other_top = xdg_surface_get_toplevel(other_xsurface);
                xdg_toplevel_add_listener(other_top, &toplevel_listener, NULL);
                xdg_toplevel_set_app_id(other_top, "target-other");
                wl_surface_commit(other_surface);
                if (wl_display_roundtrip(display) < 0)
                    break;
                puts("window-created");
            } else if (!strncmp(request, "close-window", 12) && other_surface) {
                xdg_toplevel_destroy(other_top);
                xdg_surface_destroy(other_xsurface);
                wl_surface_destroy(other_surface);
                other_surface = NULL;
                if (wl_display_roundtrip(display) < 0)
                    break;
                puts("window-closed");
            } else if (!strncmp(request, "quit", 4))
                break;
        }
        if (fds[1].revents & (POLLERR | POLLHUP))
            break;
    }
done:
    xdg_toplevel_destroy(top);
    xdg_surface_destroy(xsurface);
    wl_surface_destroy(surface);
    if (other_surface) {
        xdg_toplevel_destroy(other_top);
        xdg_surface_destroy(other_xsurface);
        wl_surface_destroy(other_surface);
    }
    for (int i = 0; i < input_count; ++i) {
        if (shapes[i])
            wp_cursor_shape_device_v1_destroy(shapes[i]);
        wl_pointer_release(pointers[i]);
        wl_keyboard_release(keyboards[i]);
    }
    if (shape_manager)
        wp_cursor_shape_manager_v1_destroy(shape_manager);
    wl_seat_release(seat);
    xdg_wm_base_destroy(shell);
    wl_shm_destroy(shm);
    if (dmabuf)
        zwp_linux_dmabuf_v1_destroy(dmabuf);
    wl_compositor_destroy(compositor);
    wl_registry_destroy(registry);
    wl_display_flush(display);
    wl_display_disconnect(display);
    if (state)
        xkb_state_unref(state);
    if (keymap)
        xkb_keymap_unref(keymap);
    xkb_context_unref(context);
    if (gbm)
        gbm_device_destroy(gbm);
    if (render_fd >= 0)
        close(render_fd);
    return 0;
}

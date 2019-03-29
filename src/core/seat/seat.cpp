#include "seat.hpp"
#include "core.hpp"
#include "input-manager.hpp"
#include "render-manager.hpp"
#include "debug.hpp"
#include "signal-definitions.hpp"
#include "../../view/priv-view.hpp"

extern "C"
{
#include <wlr/backend/libinput.h>
}

static void handle_drag_icon_map(wl_listener* listener, void *data)
{
    auto wlr_icon = (wlr_drag_icon*) data;
    auto icon = wf_surface_from_void(wlr_icon->data);
    icon->map(wlr_icon->surface);
}

static void handle_drag_icon_unmap(wl_listener* listener, void *data)
{
    auto wlr_icon = (wlr_drag_icon*) data;
    auto icon = wf_surface_from_void(wlr_icon->data);
    icon->unmap();
}

static void handle_drag_icon_destroy(wl_listener*, void *)
{
    /* we don't dec_keep_count() because the surface memory is
     * managed by the unique_ptr */
    core->input->drag_icon = nullptr;
    core->emit_signal("drag-stopped", nullptr);
}

wf_drag_icon::wf_drag_icon(wlr_drag_icon *ic)
    : wayfire_surface_t(nullptr), icon(ic)
{
    map_ev.notify   = handle_drag_icon_map;
    unmap_ev.notify = handle_drag_icon_unmap;
    destroy.notify  = handle_drag_icon_destroy;

    wl_signal_add(&icon->events.map, &map_ev);
    wl_signal_add(&icon->events.unmap, &unmap_ev);
    wl_signal_add(&icon->events.destroy, &destroy);

    icon->data = this;
}

wf_point wf_drag_icon::get_output_position()
{
    auto pos = icon->drag->grab_type == WLR_DRAG_GRAB_KEYBOARD_TOUCH ?
        core->get_touch_position(icon->drag->touch_id) : core->get_cursor_position();

    GetTuple(x, y, pos);

    if (is_mapped())
    {
        x += icon->surface->sx;
        y += icon->surface->sy;
    }

    if (output)
    {
        auto og = output->get_layout_geometry();
        x -= og.x;
        y -= og.y;
    }

    return {x, y};
}

void wf_drag_icon::damage(const wlr_box& box)
{
    if (!is_mapped())
        return;

    core->for_each_output([=] (wayfire_output *output)
    {
        auto output_geometry = output->get_layout_geometry();
        if (output_geometry & box)
        {
            auto local = box;
            local.x -= output_geometry.x;
            local.y -= output_geometry.y;

            const auto& fb = output->render->get_target_framebuffer();
            output->render->damage(fb.damage_box_from_geometry_box(local));
        }
    });
}

static void handle_request_start_drag_cb(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_seat_request_start_drag_event*> (data);
    auto seat = core->get_current_seat();

    if (wlr_seat_validate_pointer_grab_serial(seat, ev->origin, ev->serial)) {
        wlr_seat_start_pointer_drag(seat, ev->drag, ev->serial);
        return;
    }

    struct wlr_touch_point *point;
    if (wlr_seat_validate_touch_grab_serial(seat, ev->origin, ev->serial, &point)) {
        wlr_seat_start_touch_drag(seat, ev->drag, ev->serial, point);
        return;
    }

    log_debug("Ignoring start_drag request: "
            "could not validate pointer or touch serial %" PRIu32, ev->serial);
    wlr_data_source_destroy(ev->drag->source);
}

static void handle_start_drag_cb(wl_listener*, void *data)
{
    auto d = static_cast<wlr_drag*> (data);
    core->input->drag_icon = std::make_unique<wf_drag_icon> (d->icon);
    core->emit_signal("drag-started", nullptr);
}

static void handle_request_set_cursor(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_seat_pointer_request_set_cursor_event*> (data);
    core->input->cursor->set_cursor(ev);
}

static void handle_request_set_selection_cb(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_seat_request_set_selection_event*> (data);
    wlr_seat_set_selection(core->get_current_seat(), ev->source, ev->serial);
}

static void handle_request_set_primary_selection_cb(wl_listener*, void *data)
{
    auto ev = static_cast<wlr_seat_request_set_primary_selection_event*> (data);
    wlr_seat_set_primary_selection(core->get_current_seat(), ev->source, ev->serial);
}

void input_manager::update_drag_icon()
{
    if (drag_icon && drag_icon->is_mapped())
        drag_icon->update_output_position();
}

void input_manager::create_seat()
{
    cursor = std::make_unique<wf_cursor> ();

    request_set_cursor.notify = handle_request_set_cursor;
    wl_signal_add(&seat->events.request_set_cursor, &request_set_cursor);

    request_start_drag.notify = handle_request_start_drag_cb;
    wl_signal_add(&seat->events.request_start_drag, &request_start_drag);

    start_drag.notify         = handle_start_drag_cb;
    wl_signal_add(&seat->events.start_drag, &start_drag);

    request_set_selection.notify = handle_request_set_selection_cb;
    wl_signal_add(&seat->events.request_set_selection, &request_set_selection);

    request_set_primary_selection.notify = handle_request_set_primary_selection_cb;
    wl_signal_add(&seat->events.request_set_primary_selection, &request_set_primary_selection);
}

namespace wf
{
    wlr_input_device* input_device_t::get_wlr_handle()
    {
        return handle;
    }

    bool input_device_t::set_enabled(bool enabled)
    {
        if (enabled == is_enabled())
            return true;

        if (!wlr_input_device_is_libinput(handle))
            return false;

        auto dev = wlr_libinput_get_device_handle(handle);
        assert(dev);

        libinput_device_config_send_events_set_mode(dev,
            enabled ?  LIBINPUT_CONFIG_SEND_EVENTS_ENABLED :
                LIBINPUT_CONFIG_SEND_EVENTS_DISABLED);

        return true;
    }

    bool input_device_t::is_enabled()
    {
        /* Currently no support for enabling/disabling non-libinput devices */
        if (!wlr_input_device_is_libinput(handle))
            return true;

        auto dev = wlr_libinput_get_device_handle(handle);
        assert(dev);

        auto mode = libinput_device_config_send_events_get_mode(dev);
        return mode == LIBINPUT_CONFIG_SEND_EVENTS_ENABLED;
    }

    input_device_t::input_device_t(wlr_input_device *handle)
    {
        this->handle = handle;
    }
}


wf_input_device_internal::config_t wf_input_device_internal::config;
void wf_input_device_internal::config_t::load(wayfire_config *config)
{
    auto section = (*config)["input"];
    mouse_cursor_speed              = section->get_option("mouse_cursor_speed", "0");
    touchpad_cursor_speed           = section->get_option("touchpad_cursor_speed", "0");
    touchpad_tap_enabled            = section->get_option("tap_to_click", "1");
    touchpad_click_method           = section->get_option("click_method", "default");
    touchpad_scroll_method          = section->get_option("scroll_method", "default");
    touchpad_dwt_enabled            = section->get_option("disable_while_typing", "0");
    touchpad_dwmouse_enabled        = section->get_option("disable_touchpad_while_mouse", "0");
    touchpad_natural_scroll_enabled = section->get_option("natural_scroll", "0");
}

static void handle_device_destroy_cb(wl_listener *listener, void*)
{
    wf_input_device_internal::wlr_wrapper *wrapper = wl_container_of(listener, wrapper, destroy);
    core->input->handle_input_destroyed(wrapper->self->get_wlr_handle());
}

static void handle_device_switch_cb(wl_listener *listener, void* data)
{
    wf_input_device_internal::wlr_wrapper *wrapper = wl_container_of(listener, wrapper, switched);
    auto ev = (wlr_event_switch_toggle*) data;
    wrapper->self->handle_switched(ev);
}

wf_input_device_internal::wf_input_device_internal(wlr_input_device *dev)
    : wf::input_device_t(dev)
{
    update_options();

    _wrapper.self = this;
    _wrapper.destroy.notify = handle_device_destroy_cb;

    wl_signal_add(&dev->events.destroy, &_wrapper.destroy);

    if (dev->type == WLR_INPUT_DEVICE_SWITCH)
    {
        _wrapper.switched.notify = handle_device_switch_cb;
        wl_signal_add(&dev->switch_device->events.toggle, &_wrapper.switched);
    } else
    {
        _wrapper.switched.notify = nullptr;
    }
}

wf_input_device_internal::~wf_input_device_internal()
{
    if (_wrapper.switched.notify)
        wl_list_remove(&_wrapper.switched.link);
    wl_list_remove(&_wrapper.destroy.link);
}

void wf_input_device_internal::handle_switched(wlr_event_switch_toggle *ev)
{
    wf::switch_signal data;
    data.device = nonstd::make_observer(this);
    data.state = (ev->switch_state == WLR_SWITCH_STATE_ON);

    std::string event_name;
    switch (ev->switch_type)
    {
        case WLR_SWITCH_TYPE_TABLET_MODE:
            event_name = "tablet-mode";
            break;
        case WLR_SWITCH_TYPE_LID:
            event_name = "lid-state";
            break;
    }

    core->emit_signal(event_name, &data);
}

void wf_input_device_internal::update_options()
{
    /* We currently support options only for libinput devices */
    if (!wlr_input_device_is_libinput(get_wlr_handle()))
        return;

    auto dev = wlr_libinput_get_device_handle(get_wlr_handle());
    assert(dev);

    /* we are configuring a touchpad */
    if (libinput_device_config_tap_get_finger_count(dev) > 0)
    {
        libinput_device_config_accel_set_speed(dev,
            config.touchpad_cursor_speed->as_cached_double());

        libinput_device_config_tap_set_enabled(dev,
            config.touchpad_tap_enabled->as_cached_int() ?
            LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);

        if (config.touchpad_click_method->as_string() == "default") {
            libinput_device_config_click_set_method(dev,
                libinput_device_config_click_get_default_method(dev));
        } else if (config.touchpad_click_method->as_string() == "none") {
            libinput_device_config_click_set_method(dev,
                LIBINPUT_CONFIG_CLICK_METHOD_NONE);
        } else if (config.touchpad_click_method->as_string() == "button-areas") {
            libinput_device_config_click_set_method(dev,
                LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS);
        } else if (config.touchpad_click_method->as_string() == "clickfinger") {
            libinput_device_config_click_set_method(dev,
                LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER);
        }

        if (config.touchpad_scroll_method->as_string() == "default") {
            libinput_device_config_scroll_set_method(dev,
                libinput_device_config_scroll_get_default_method(dev));
        } else if (config.touchpad_scroll_method->as_string() == "none") {
            libinput_device_config_scroll_set_method(dev,
                LIBINPUT_CONFIG_SCROLL_NO_SCROLL);
        } else if (config.touchpad_scroll_method->as_string() == "two-finger") {
            libinput_device_config_scroll_set_method(dev,
                LIBINPUT_CONFIG_SCROLL_2FG);
        } else if (config.touchpad_scroll_method->as_string() == "edge") {
            libinput_device_config_scroll_set_method(dev,
                LIBINPUT_CONFIG_SCROLL_EDGE);
        } else if (config.touchpad_scroll_method->as_string() == "on-button-down") {
            libinput_device_config_scroll_set_method(dev,
                LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN);
        }

        libinput_device_config_dwt_set_enabled(dev,
            config.touchpad_dwt_enabled->as_cached_int() ?
            LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);

        libinput_device_config_send_events_set_mode(dev,
            config.touchpad_dwmouse_enabled->as_cached_int() ?
            LIBINPUT_CONFIG_SEND_EVENTS_DISABLED_ON_EXTERNAL_MOUSE
                : LIBINPUT_CONFIG_SEND_EVENTS_ENABLED);

        if (libinput_device_config_scroll_has_natural_scroll(dev) > 0)
        {
            libinput_device_config_scroll_set_natural_scroll_enabled(dev,
                    (bool)config.touchpad_natural_scroll_enabled->as_cached_int());
        }
    } else {
        libinput_device_config_accel_set_speed(dev,
            config.mouse_cursor_speed->as_cached_double());
    }
}

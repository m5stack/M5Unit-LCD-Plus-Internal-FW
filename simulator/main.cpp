#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <SDL2/SDL.h>
#include <lvgl.h>
#include <src/drivers/sdl/lv_sdl_window.h>

#include "unit_lcd_ui.hpp"

namespace
{
  constexpr std::int32_t DISPLAY_WIDTH = 135;
  constexpr std::int32_t DISPLAY_HEIGHT = 240;
  constexpr std::uint32_t SAVE_DELAY_MS = 650;

  struct options_t
  {
    unit_lcd_ui::variant_t variant = unit_lcd_ui::variant_t::utility;
    unit_lcd_ui::preview_state_t state = unit_lcd_ui::preview_state_t::home;
    const char* capture_path = nullptr;
    float zoom = 3.0F;
    bool save_succeeds = true;
    bool report_memory = false;
    bool smoke = false;
    std::uint8_t firmware_version = 1;
  };

  struct app_t
  {
    unit_lcd_ui::controller_t ui;
    std::array<unit_lcd_ui::action_t, 32> actions = {};
    std::size_t action_read = 0;
    std::size_t action_write = 0;
    std::uint32_t save_due = 0;
    bool save_succeeds = true;
    std::uint32_t address_requests = 0;
    std::uint32_t theme_changes = 0;
    unit_lcd_ui::variant_t last_theme = unit_lcd_ui::variant_t::utility;
    std::uint8_t requested_address = 0;
    std::atomic<bool> running = true;

    void push(unit_lcd_ui::action_t action)
    {
      const auto next = (action_write + 1) % actions.size();
      if (next == action_read) return;
      actions[action_write] = action;
      action_write = next;
    }

    bool pop(unit_lcd_ui::action_t* action)
    {
      if (action == nullptr || action_read == action_write) return false;
      *action = actions[action_read];
      action_read = (action_read + 1) % actions.size();
      return true;
    }
  };

  bool parse_state(const char* value, unit_lcd_ui::preview_state_t* state)
  {
    if (value == nullptr || state == nullptr) return false;
    struct mapping_t
    {
      const char* name;
      unit_lcd_ui::preview_state_t state;
    };
    constexpr mapping_t mappings[] = {
      { "boot", unit_lcd_ui::preview_state_t::boot_logo },
      { "home", unit_lcd_ui::preview_state_t::home },
      { "pressed", unit_lcd_ui::preview_state_t::pressed },
      { "edit", unit_lcd_ui::preview_state_t::address_edit },
      { "adjust", unit_lcd_ui::preview_state_t::address_adjust },
      { "confirm", unit_lcd_ui::preview_state_t::address_confirm },
      { "saving", unit_lcd_ui::preview_state_t::address_saving },
      { "saved", unit_lcd_ui::preview_state_t::address_saved },
      { "error", unit_lcd_ui::preview_state_t::save_error },
      { "about", unit_lcd_ui::preview_state_t::about },
      { "theme", unit_lcd_ui::preview_state_t::theme },
    };
    for (const auto& mapping : mappings)
    {
      if (std::strcmp(value, mapping.name) == 0)
      {
        *state = mapping.state;
        return true;
      }
    }
    return false;
  }

  void print_usage(const char* executable)
  {
    std::printf(
      "Usage: %s [--variant light|dark] "
      "[--state boot|home|pressed|edit|adjust|confirm|saving|saved|error|about|theme] "
      "[--capture FILE.bmp] [--firmware-version 0..127] [--zoom SCALE] [--fail-save] [--stats] [--smoke]\n",
      executable);
  }

  bool parse_options(int argc, char** argv, options_t* options)
  {
    for (int i = 1; i < argc; ++i)
    {
      if (std::strcmp(argv[i], "--variant") == 0 && i + 1 < argc)
      {
        if (!unit_lcd_ui::parse_variant(argv[++i], &options->variant)) return false;
      }
      else if (std::strcmp(argv[i], "--state") == 0 && i + 1 < argc)
      {
        if (!parse_state(argv[++i], &options->state)) return false;
      }
      else if (std::strcmp(argv[i], "--capture") == 0 && i + 1 < argc)
      {
        options->capture_path = argv[++i];
      }
      else if (std::strcmp(argv[i], "--firmware-version") == 0 && i + 1 < argc)
      {
        char* end = nullptr;
        const auto version = std::strtoul(argv[++i], &end, 10);
        if (end == argv[i] || *end != '\0' || version > 127) return false;
        options->firmware_version = static_cast<std::uint8_t>(version);
      }
      else if (std::strcmp(argv[i], "--zoom") == 0 && i + 1 < argc)
      {
        options->zoom = std::strtof(argv[++i], nullptr);
        if (options->zoom < 1.0F || options->zoom > 8.0F) return false;
      }
      else if (std::strcmp(argv[i], "--fail-save") == 0)
      {
        options->save_succeeds = false;
      }
      else if (std::strcmp(argv[i], "--stats") == 0)
      {
        options->report_memory = true;
      }
      else if (std::strcmp(argv[i], "--smoke") == 0)
      {
        options->smoke = true;
      }
      else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0)
      {
        print_usage(argv[0]);
        std::exit(EXIT_SUCCESS);
      }
      else
      {
        return false;
      }
    }
    if (options->capture_path != nullptr) options->zoom = 1.0F;
    return true;
  }

  void address_change_requested(std::uint8_t address, void* user_data)
  {
    auto* app = static_cast<app_t*>(user_data);
    ++app->address_requests;
    app->requested_address = address;
    app->save_due = SDL_GetTicks() + SAVE_DELAY_MS;
  }

  void theme_changed(unit_lcd_ui::variant_t variant, void* user_data)
  {
    auto* app = static_cast<app_t*>(user_data);
    ++app->theme_changes;
    app->last_theme = variant;
  }

  int event_watch(void* user_data, SDL_Event* event)
  {
    auto* app = static_cast<app_t*>(user_data);
    if (event->type == SDL_QUIT)
    {
      app->running = false;
      return 1;
    }

    if (event->type == SDL_MOUSEWHEEL)
    {
      if (event->wheel.y > 0) app->push(unit_lcd_ui::action_t::previous);
      if (event->wheel.y < 0) app->push(unit_lcd_ui::action_t::next);
      return 1;
    }

    if (event->type == SDL_MOUSEBUTTONDOWN && event->button.button == SDL_BUTTON_LEFT)
    {
      app->push(unit_lcd_ui::action_t::press);
      return 1;
    }
    if (event->type == SDL_MOUSEBUTTONUP && event->button.button == SDL_BUTTON_LEFT)
    {
      app->push(unit_lcd_ui::action_t::release);
      return 1;
    }

    if (event->type == SDL_KEYDOWN)
    {
      switch (event->key.keysym.sym)
      {
        case SDLK_UP:
        case SDLK_LEFT:
        case SDLK_PAGEUP:
          app->push(unit_lcd_ui::action_t::previous);
          break;
        case SDLK_DOWN:
        case SDLK_RIGHT:
        case SDLK_PAGEDOWN:
          app->push(unit_lcd_ui::action_t::next);
          break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
          if (event->key.repeat == 0) app->push(unit_lcd_ui::action_t::press);
          break;
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE:
          if (event->key.repeat == 0) app->push(unit_lcd_ui::action_t::long_press);
          break;
        case SDLK_q:
          app->running = false;
          break;
        default:
          break;
      }
    }
    else if (event->type == SDL_KEYUP)
    {
      switch (event->key.keysym.sym)
      {
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
          app->push(unit_lcd_ui::action_t::release);
          break;
        default:
          break;
      }
    }
    return 1;
  }

  int event_filter(void* user_data, SDL_Event* event)
  {
    auto* app = static_cast<app_t*>(user_data);
    if (event->type == SDL_QUIT)
    {
      app->running = false;
      return 0;
    }
    return 1;
  }

  bool save_capture(lv_display_t* display, const char* path)
  {
    lv_refr_now(display);
    auto* renderer = static_cast<SDL_Renderer*>(lv_sdl_window_get_renderer(display));
    if (renderer == nullptr) return false;

    int width = 0;
    int height = 0;
    if (SDL_GetRendererOutputSize(renderer, &width, &height) != 0) return false;

    auto* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
                                                    SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) return false;
    const auto read_result = SDL_RenderReadPixels(renderer, nullptr,
                                                   SDL_PIXELFORMAT_ARGB8888,
                                                   surface->pixels, surface->pitch);
    const auto save_result = read_result == 0 ? SDL_SaveBMP(surface, path) : -1;
    SDL_FreeSurface(surface);
    return save_result == 0;
  }

  lv_obj_t* find_label(lv_obj_t* object, const char* text)
  {
    if (lv_obj_check_type(object, &lv_label_class)
        && std::strcmp(lv_label_get_text(object), text) == 0) return object;
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
      if (auto* found = find_label(lv_obj_get_child(object, i), text)) return found;
    return nullptr;
  }

  bool has_label(lv_obj_t* object, const char* text)
  {
    if (lv_obj_check_type(object, &lv_label_class)
        && std::strcmp(lv_label_get_text(object), text) == 0) return true;
    for (std::uint32_t i = 0; i < lv_obj_get_child_count(object); ++i)
    {
      if (has_label(lv_obj_get_child(object, i), text)) return true;
    }
    return false;
  }

  bool validate_layout(lv_obj_t* object)
  {
    bool valid = true;
    // Rolling digit columns (USER_1) intentionally keep their wrapped label
    // list outside the parent's clip box; those coordinates are not defects.
    if (lv_obj_has_flag(object, LV_OBJ_FLAG_USER_1)) return true;
    if (object != lv_screen_active())
    {
      lv_area_t area;
      lv_obj_get_coords(object, &area);
      if (area.x1 < 0 || area.y1 < 0 || area.x2 >= DISPLAY_WIDTH || area.y2 >= DISPLAY_HEIGHT)
      {
        std::fprintf(stderr, "Object out of bounds: (%ld,%ld)-(%ld,%ld)\n",
                     static_cast<long>(area.x1), static_cast<long>(area.y1),
                     static_cast<long>(area.x2), static_cast<long>(area.y2));
        valid = false;
      }
    }

    if (lv_obj_check_type(object, &lv_label_class))
    {
      lv_point_t text_size;
      lv_text_get_size(&text_size, lv_label_get_text(object),
                       lv_obj_get_style_text_font(object, LV_PART_MAIN),
                       lv_obj_get_style_text_letter_space(object, LV_PART_MAIN),
                       lv_obj_get_style_text_line_space(object, LV_PART_MAIN),
                       LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      const auto available_width = lv_obj_get_content_width(object);
      if (text_size.x > available_width)
      {
        std::fprintf(stderr, "Label clipped: '%s' needs %ld px, has %ld px\n",
                     lv_label_get_text(object), static_cast<long>(text_size.x),
                     static_cast<long>(available_width));
        valid = false;
      }
    }

    const auto child_count = lv_obj_get_child_count(object);
    for (std::uint32_t i = 0; i < child_count; ++i)
    {
      if (!validate_layout(lv_obj_get_child(object, static_cast<std::int32_t>(i)))) valid = false;
    }
    return valid;
  }
}

int main(int argc, char** argv)
{
  options_t options;
  if (!parse_options(argc, argv, &options))
  {
    print_usage(argv[0]);
    return EXIT_FAILURE;
  }

  lv_init();
  auto* display = lv_sdl_window_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
  if (display == nullptr)
  {
    std::fprintf(stderr, "Unable to create LVGL SDL display\n");
    return EXIT_FAILURE;
  }
  lv_sdl_window_set_zoom(display, options.zoom);

  std::string title = "Unit LCD Plus UI / ";
  title += unit_lcd_ui::variant_name(options.variant);
  lv_sdl_window_set_title(display, title.c_str());

  app_t app;
  app.save_succeeds = options.save_succeeds;
  unit_lcd_ui::device_info_t info;
  info.address = 0x3E;
  info.firmware_minor = options.firmware_version;
  info.debug_build = false;
  unit_lcd_ui::callbacks_t callbacks;
  callbacks.request_address_change = address_change_requested;
  callbacks.theme_changed = theme_changed;
  callbacks.user_data = &app;
  if (!app.ui.create(lv_screen_active(), options.variant, info, callbacks))
  {
    std::fprintf(stderr, "Unable to create UI\n");
    return EXIT_FAILURE;
  }
  app.ui.show_preview_state(options.state);
  if (options.report_memory)
  {
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    std::printf("LVGL heap: %zu used, %zu peak, %zu free, %u%% fragmented\n",
                memory.total_size - memory.free_size, memory.max_used,
                memory.free_size, static_cast<unsigned>(memory.frag_pct));
  }
  SDL_SetEventFilter(event_filter, &app);
  SDL_AddEventWatch(event_watch, &app);

  bool smoke_passed = true;
  if (options.smoke)
  {
    if (options.capture_path != nullptr || options.state != unit_lcd_ui::preview_state_t::home)
    {
      std::fprintf(stderr, "--smoke requires the default home state and no --capture\n");
      smoke_passed = false;
    }
    else
    {
      auto pump = [&](std::uint32_t duration_ms)
      {
        const auto pump_start = SDL_GetTicks();
        while (SDL_GetTicks() - pump_start < duration_ms)
        {
          lv_timer_handler();
          const auto now = SDL_GetTicks();
          if (app.save_due != 0 && static_cast<std::int32_t>(now - app.save_due) >= 0)
          {
            app.save_due = 0;
            app.ui.set_address_save_result(app.save_succeeds);
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      };
      auto check = [&](bool condition, const char* scenario)
      {
        if (!condition) std::fprintf(stderr, "SMOKE failed: %s\n", scenario);
        smoke_passed = smoke_passed && condition;
      };
      app.ui.show_boot_logo();
      char boot_address[8];
      char boot_version[12];
      std::snprintf(boot_address, sizeof(boot_address), "0x%02X", info.address);
      std::snprintf(boot_version, sizeof(boot_version), "V%u.%u", info.firmware_major, info.firmware_minor);
      check(has_label(lv_screen_active(), boot_address) && has_label(lv_screen_active(), boot_version),
            "boot displays current address and actual firmware version");
      app.ui.dispatch(unit_lcd_ui::action_t::press);
      app.ui.dispatch(unit_lcd_ui::action_t::release);
      app.ui.dispatch(unit_lcd_ui::action_t::next);
      pump(600);
      check(!has_label(lv_screen_active(), "Unit LCD-Plus") && app.address_requests == 0
            && app.theme_changes == 0, "boot logo holds and ignores input");
      pump(500);
      check(has_label(lv_screen_active(), "Unit LCD-Plus"), "boot advances to home");
      app.ui.dispatch(unit_lcd_ui::action_t::release);
      check(has_label(lv_screen_active(), "Unit LCD-Plus"), "boot release does not activate");
      lv_obj_update_layout(lv_screen_active());
      auto* initial_title = find_label(lv_screen_active(), "Unit LCD-Plus");
      const auto* title_font = lv_obj_get_style_text_font(initial_title, LV_PART_MAIN);
      const auto title_y = lv_obj_get_y(initial_title);
      auto check_header = [&]
      {
        lv_obj_update_layout(lv_screen_active());
        auto* title = find_label(lv_screen_active(), "Unit LCD-Plus");
        if (title == nullptr) title = find_label(lv_screen_active(), "SETTING");
        if (title == nullptr) title = find_label(lv_screen_active(), "Device Info");
        check(title != nullptr, "header exists");
        if (title == nullptr) return;
        auto* content = lv_obj_get_parent(title);
        check(lv_obj_get_x(content) == 0 && lv_obj_get_y(content) == 0, "page never translated");
        check(lv_obj_get_y(title) == title_y && lv_obj_get_style_text_font(title, LV_PART_MAIN) == title_font,
              "fixed title font and y");
        bool divider = false;
        for (std::uint32_t i = 0; i < lv_obj_get_child_count(content); ++i)
        {
          auto* child = lv_obj_get_child(content, i);
          if (lv_obj_get_y(child) == 29 && lv_obj_get_height(child) == 1
              && lv_obj_get_width(child) == DISPLAY_WIDTH) divider = true;
        }
        check(divider, "fixed header divider");
      };
      auto click = [&]
      {
        app.ui.dispatch(unit_lcd_ui::action_t::press);
        app.ui.dispatch(unit_lcd_ui::action_t::release);
        check_header();                                  // Entry frame
        pump(80);
        check_header();                                  // Former transition midpoint
        pump(120);
        check_header();                                  // Settled frame
      };
      auto check_address = [&](bool editing)
      {
        lv_obj_update_layout(lv_screen_active());
        auto* left = find_label(lv_screen_active(), "<");
        check(left != nullptr, "address slots exist");
        if (left == nullptr) return;
        auto* group = lv_obj_get_parent(left);
        const auto width = lv_obj_get_width(left);
        check(lv_obj_get_width(group) == 6 * width
              && std::abs(2 * lv_obj_get_x(group) + 6 * width - DISPLAY_WIDTH) <= 1,
              "address centered equal slots");
        const auto palette = app.theme_changes ? app.last_theme : options.variant;
        const bool dark = palette == unit_lcd_ui::variant_t::precision;
        const auto expected = lv_color_to_u32(lv_color_hex(editing ? (dark ? 0x49D17D : 0x238A55)
                                                                : (dark ? 0xF2F6F5 : 0x17201E)));
        for (std::uint32_t i = 0; i < 6; ++i)
        {
          auto* glyph = lv_obj_get_child(group, i);
          check(lv_obj_get_x(glyph) == static_cast<int>(i) * width
                && lv_obj_get_y(glyph) == 0 && lv_obj_get_width(glyph) == width,
                "equal address character placement");
          check(lv_color_to_u32(lv_obj_get_style_text_color(glyph, LV_PART_MAIN)) == expected,
                "uniform address color");
        }
        for (std::uint32_t i = 6; i < lv_obj_get_child_count(group); ++i)
        {
          auto* column = lv_obj_get_child(group, i);
          auto* glyph = lv_obj_get_child(column, 0);
          check(lv_obj_get_width(column) == width
                && lv_color_to_u32(lv_obj_get_style_text_color(glyph, LV_PART_MAIN)) == expected,
                "rolling slot width and color");
        }
      };
      auto visible = [&](const char* text) { return has_label(lv_screen_active(), text); };
      using action = unit_lcd_ui::action_t;
      auto dispatch = [&](action value) { app.ui.dispatch(value); };
      // Probe editing through input behavior, not instructional text.
      auto adjusting = [&]
      {
        const auto before = app.ui.pending_address();
        const bool down = before > 0x08;
        dispatch(down ? action::previous : action::next);
        const bool changed = app.ui.pending_address() != before;
        dispatch(down ? action::next : action::previous);
        return changed;
      };
      auto hold = [&]
      {
        dispatch(action::press);
        dispatch(action::long_press);
        dispatch(action::release);
        pump(200);
      };
      check(visible("Unit LCD-Plus"), "home title");
      hold();
      check(visible("Unit LCD-Plus"), "home hold must not activate");
      dispatch(action::previous);                       // Device
      click();
      char version[12];
      std::snprintf(version, sizeof(version), "V%u.%u", info.firmware_major, info.firmware_minor);
      check(visible("SoC") && visible("4MiB") && visible(version), "device info");
      hold();
      check(visible("SoC"), "device hold must not go back");
      click();                                         // Explicit Back
      click();
      check(visible("SoC"), "device return focus");
      click();
      dispatch(action::previous);                       // Theme
      click();
      check(visible("Light") && visible("Dark") && visible("Back"), "theme has Back");
      click();
      check(app.theme_changes == 0, "unchanged theme");
      // Toggle between the two palettes, avoiding the third Back item.
      dispatch(options.variant == unit_lcd_ui::variant_t::precision ? action::previous : action::next);
      hold();
      check(app.theme_changes == 0 && visible("Light"), "theme hold does not apply or exit");
      click();
      const auto expected_theme = options.variant == unit_lcd_ui::variant_t::precision
                                ? unit_lcd_ui::variant_t::utility
                                : unit_lcd_ui::variant_t::precision;
      check(app.theme_changes == 1 && app.last_theme == expected_theme, "theme mapping");
      if (expected_theme == unit_lcd_ui::variant_t::utility) dispatch(action::previous);
      else dispatch(action::next);
      hold();
      check(visible("Light"), "holding Back must not exit");
      click();
      check(visible("Unit LCD-Plus"), "theme Back");
      click();
      check(visible("Light"), "theme return focus");
      if (expected_theme == unit_lcd_ui::variant_t::utility) dispatch(action::previous);
      else dispatch(action::next);
      click();
      dispatch(action::previous);                       // Address
      click();
      check((visible("Current") && !adjusting()) && visible("Back"), "address navigation state");
      check_address(false);
      const auto original = app.ui.pending_address();
      dispatch(action::next);                           // Focus Back, no value change
      check(app.ui.pending_address() == original, "navigation must not edit address");
      pump(250);
      check(validate_layout(lv_screen_active()), "address Back focus layout");
      hold();
      check(visible("Current"), "holding address Back must not exit");
      click();
      check(visible("Unit LCD-Plus"), "address Back");
      click();
      hold();
      check((visible("Current") && !adjusting()), "hold does not enter edit");
      click();
      check((visible("Current") && adjusting()), "click starts adjustment");
      check_address(true);
      hold();
      check((visible("Current") && adjusting()) && app.address_requests == 0, "hold does not confirm");
      for (int i = 0; i < 160; ++i) dispatch(action::previous);
      check(app.ui.pending_address() == 0x08, "lower bound");
      for (int i = 0; i < 160; ++i) dispatch(action::next);
      check(app.ui.pending_address() == 0x77, "upper bound");
      pump(400);
      check_address(true);
      check(validate_layout(lv_screen_active()), "adjust layout");
      click();
      check(visible("Applying\nwill reboot!") && app.address_requests == 0, "confirmation does not save");
      hold();
      check(visible("Applying\nwill reboot!") && app.address_requests == 0, "hold does not apply");
      dispatch(action::next);
      click();                                         // Cancel -> address selection
      check((visible("Current") && !adjusting()) && app.ui.pending_address() == 0x77, "Cancel returns to field");
      check_address(false);
      dispatch(action::next);
      click();                                         // Back discards pending edit
      click();
      check(app.ui.pending_address() == original, "Back discards edit");
      click();                                         // Start adjustment
      click();                                         // Confirm unchanged address
      check(visible("Applying\nwill reboot!") && app.address_requests == 0, "unchanged confirm");
      dispatch(action::next);
      click();                                         // Cancel
      click();                                         // Start adjustment
      dispatch(action::next);                           // 0x3F
      click();
      click();                                         // Apply
      pump(800);
      check(app.address_requests == 1 && app.requested_address == 0x3F, "one save");
      check(visible(app.save_succeeds ? "SAVED" : "NOT SAVED"), "save result");
      if (!app.save_succeeds)
      {
        click();                                       // Retry -> address selection
        check((visible("Current") && !adjusting()) && app.ui.pending_address() == 0x3F, "retry retains value");
        click();                                       // Start adjustment
        click();                                       // Confirm
        click();                                       // Apply
        pump(800);
        check(app.address_requests == 2 && visible("NOT SAVED"), "retry save");
      }
      pump(200);
      check(validate_layout(lv_screen_active()), "final layout");
    }
    {
      lv_mem_monitor_t memory;
      lv_mem_monitor(&memory);
      std::printf("SMOKE %s; LVGL heap peak %zu, free %zu\n",
                  smoke_passed ? "PASS" : "FAIL", memory.max_used,
                  memory.free_size);
    }
    app.running = false;
  }

  const auto start = SDL_GetTicks();
  bool captured = false;
  bool layout_valid = true;
  while (app.running)
  {
    const auto wait = lv_timer_handler();
    unit_lcd_ui::action_t action;
    while (app.pop(&action)) app.ui.dispatch(action);

    const auto now = SDL_GetTicks();
    if (app.save_due != 0 && static_cast<std::int32_t>(now - app.save_due) >= 0)
    {
      app.save_due = 0;
      app.ui.set_address_save_result(app.save_succeeds);
    }

    if (options.capture_path != nullptr && now - start >= 120)
    {
      lv_obj_update_layout(lv_screen_active());
      layout_valid = validate_layout(lv_screen_active());
      captured = save_capture(display, options.capture_path);
      app.running = false;
    }
    else
    {
      const auto delay_ms = std::max<std::uint32_t>(1, std::min<std::uint32_t>(wait, 16));
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
  }

  SDL_DelEventWatch(event_watch, &app);
  SDL_SetEventFilter(nullptr, nullptr);
  app.ui.destroy();
  lv_sdl_quit();
  lv_deinit();

  if (options.capture_path != nullptr && (!captured || !layout_valid))
  {
    if (!captured)
    {
      std::fprintf(stderr, "Unable to save capture: %s (%s)\n",
                   options.capture_path, SDL_GetError());
    }
    if (!layout_valid) std::fprintf(stderr, "Layout validation failed\n");
    return EXIT_FAILURE;
  }
  return smoke_passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

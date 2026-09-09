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
      { "home", unit_lcd_ui::preview_state_t::home },
      { "pressed", unit_lcd_ui::preview_state_t::pressed },
      { "edit", unit_lcd_ui::preview_state_t::address_edit },
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
      "Usage: %s [--variant precision|utility|console] "
      "[--state home|pressed|edit|confirm|saving|saved|error|about|theme] "
      "[--capture FILE.bmp] [--zoom SCALE] [--fail-save] [--stats] [--smoke]\n",
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
          if (event->key.repeat == 0) app->push(unit_lcd_ui::action_t::back);
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
  info.firmware_version = 5;
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
      auto click = [&]
      {
        app.ui.dispatch(unit_lcd_ui::action_t::press);
        app.ui.dispatch(unit_lcd_ui::action_t::release);
        pump(200);
      };

      app.ui.dispatch(unit_lcd_ui::action_t::next);  // Home: DEVICE
      pump(40);
      click();                                         // Device info
      click();                                         // Back to home
      app.ui.dispatch(unit_lcd_ui::action_t::next);   // Home: THEME
      pump(40);
      click();                                         // Theme picker
      app.ui.dispatch(unit_lcd_ui::action_t::next);   // Picker: next theme
      pump(40);
      click();                                         // Apply theme live
      pump(120);
      app.ui.dispatch(unit_lcd_ui::action_t::back);   // Picker -> home (THEME)
      app.ui.dispatch(unit_lcd_ui::action_t::previous); // Home: DEVICE
      app.ui.dispatch(unit_lcd_ui::action_t::previous); // Home: ADDRESS
      pump(40);
      click();                                         // Address editor
      app.ui.dispatch(unit_lcd_ui::action_t::next);   // 0x3E -> 0x3F
      pump(40);
      click();                                         // Confirm
      click();                                         // Apply / saving
      pump(800);

      const auto expected_theme = static_cast<unit_lcd_ui::variant_t>(
        (static_cast<std::uint8_t>(options.variant) + 1) % 3);
      smoke_passed = app.address_requests == 1
                   && app.requested_address == 0x3F
                   && app.ui.pending_address() == 0x3F
                   && app.theme_changes == 1
                   && app.last_theme == expected_theme;
      if (!app.save_succeeds)
      {
        click();                                       // Error: TRY AGAIN
      }
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
  return EXIT_SUCCESS;
}

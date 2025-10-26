#include <pebble.h>
#include "progress_layer.h"

// Data structure for app store items
typedef struct {
  char name[64];
  char author[64];
  char description[128];
  int hearts;
  int days_ago;
} AppStoreItem;

// Global variables
static Window *s_loading_window;
static ProgressLayer *s_progress_layer;

static Window *s_main_window;
static StatusBarLayer *s_status_bar;
static TextLayer *s_name_layer;
static TextLayer *s_author_layer;
static TextLayer *s_description_layer;
static TextLayer *s_hearts_layer;
static TextLayer *s_pagination_layer;

static int s_current_index = 0;
static Animation *s_previous_animation = NULL;
static bool s_data_loaded = false;
static int s_apps_received = 0;

// Animation constants
static const uint32_t SCROLL_DURATION = 120;
static const int16_t SCROLL_DIST_OUT = 20;
static const int16_t SCROLL_DIST_IN = 8;

typedef enum {
  ScrollDirectionDown,
  ScrollDirectionUp,
} ScrollDirection;

// App data - populated from JavaScript
static AppStoreItem s_apps[10];

static int get_num_apps(void) {
  return ARRAY_LENGTH(s_apps);
}

static AppStoreItem* get_app_at_index(int index) {
  if (index < 0 || index >= get_num_apps()) {
    return NULL;
  }
  return &s_apps[index];
}

// Format days ago integer into display string
static void format_days_ago(int days, char *buffer, size_t buffer_size) {
  if (days < 0) {
    snprintf(buffer, buffer_size, "Unknown");
  } else if (days == 0) {
    snprintf(buffer, buffer_size, "Today");
  } else if (days == 1) {
    snprintf(buffer, buffer_size, "Yesterday");
  } else {
    snprintf(buffer, buffer_size, "%d days ago", days);
  }
}

// Calculate expiration time for Sunday 23:59:59 of current week
static time_t get_sunday_expiration(void) {
  time_t now = time(NULL);
  struct tm *time_info = localtime(&now);

  // Calculate days until Sunday (0 = Sunday, 1 = Monday, ..., 6 = Saturday)
  // If today is Sunday (0), we want next Sunday (7 days)
  int days_until_sunday = (7 - time_info->tm_wday) % 7;
  if (days_until_sunday == 0) {
    days_until_sunday = 7; // If today is Sunday, expire next Sunday
  }

  // Set time to 23:59:59
  time_info->tm_hour = 23;
  time_info->tm_min = 59;
  time_info->tm_sec = 59;
  time_info->tm_mday += days_until_sunday;

  return mktime(time_info);
}

// Count how many apps were released this week (last 7 days)
static int count_new_apps_this_week(void) {
  int count = 0;
  for (int i = 0; i < get_num_apps(); i++) {
    if (s_apps[i].days_ago >= 0 && s_apps[i].days_ago <= 6) {
      count++;
    }
  }
  return count;
}

// Update the display with current app data
static void update_display(void) {
  AppStoreItem *app = get_app_at_index(s_current_index);
  if (!app) {
    return;
  }

  // Update text layers
  text_layer_set_text(s_name_layer, app->name);
  text_layer_set_text(s_author_layer, app->author);
  text_layer_set_text(s_description_layer, app->description);

  // Format hearts count and release date
  static char hearts_text[64];
  static char days_ago_text[32];
  format_days_ago(app->days_ago, days_ago_text, sizeof(days_ago_text));
  snprintf(hearts_text, sizeof(hearts_text), "❤ %d  •  %s", app->hearts, days_ago_text);
  text_layer_set_text(s_hearts_layer, hearts_text);

  // Format pagination
  static char pagination_text[16];
  snprintf(pagination_text, sizeof(pagination_text), "%d/%d", s_current_index + 1, get_num_apps());
  text_layer_set_text(s_pagination_layer, pagination_text);
}

// Update loading progress
static void update_loading_progress(int percentage) {
  if (s_progress_layer) {
    progress_layer_set_progress(s_progress_layer, percentage);
  }
}

// Animation functions
static Animation *create_anim_scroll_out(Layer *layer, uint32_t duration, int16_t dy) {
  GPoint to_origin = GPoint(0, dy);
  Animation *result = (Animation *) property_animation_create_bounds_origin(layer, NULL, &to_origin);
  animation_set_duration(result, duration);
  animation_set_curve(result, AnimationCurveLinear);
  return result;
}

static Animation *create_anim_scroll_in(Layer *layer, uint32_t duration, int16_t dy) {
  GPoint from_origin = GPoint(0, dy);
  Animation *result = (Animation *) property_animation_create_bounds_origin(layer, &from_origin, &GPointZero);
  animation_set_duration(result, duration);
  animation_set_curve(result, AnimationCurveEaseOut);
  return result;
}

static Animation *create_outbound_anim(ScrollDirection direction) {
  const int16_t to_dy = (direction == ScrollDirectionDown) ? -SCROLL_DIST_OUT : SCROLL_DIST_OUT;

  Animation *out_name = create_anim_scroll_out(text_layer_get_layer(s_name_layer), SCROLL_DURATION, to_dy);
  Animation *out_author = create_anim_scroll_out(text_layer_get_layer(s_author_layer), SCROLL_DURATION, to_dy);
  Animation *out_description = create_anim_scroll_out(text_layer_get_layer(s_description_layer), SCROLL_DURATION, to_dy);
  Animation *out_hearts = create_anim_scroll_out(text_layer_get_layer(s_hearts_layer), SCROLL_DURATION, to_dy);

  return animation_spawn_create(out_name, out_author, out_description, out_hearts, NULL);
}

static Animation *create_inbound_anim(ScrollDirection direction) {
  const int16_t from_dy = (direction == ScrollDirectionDown) ? -SCROLL_DIST_IN : SCROLL_DIST_IN;

  Animation *in_name = create_anim_scroll_in(text_layer_get_layer(s_name_layer), SCROLL_DURATION, from_dy);
  Animation *in_author = create_anim_scroll_in(text_layer_get_layer(s_author_layer), SCROLL_DURATION, from_dy);
  Animation *in_description = create_anim_scroll_in(text_layer_get_layer(s_description_layer), SCROLL_DURATION, from_dy);
  Animation *in_hearts = create_anim_scroll_in(text_layer_get_layer(s_hearts_layer), SCROLL_DURATION, from_dy);

  return animation_spawn_create(in_name, in_author, in_description, in_hearts, NULL);
}

static void after_scroll_update_text(Animation *animation, bool finished, void *context) {
  update_display();
}

static Animation *create_scroll_animation(ScrollDirection direction) {
  Animation *out_text = create_outbound_anim(direction);
  animation_set_handlers(out_text, (AnimationHandlers) {
    .stopped = after_scroll_update_text,
  }, NULL);
  Animation *in_text = create_inbound_anim(direction);

  return animation_sequence_create(out_text, in_text, NULL);
}

static Animation *create_bounce_animation(ScrollDirection direction) {
  return create_inbound_anim(direction);
}

static void scroll_to_index(int new_index, ScrollDirection direction) {
  if (new_index < 0 || new_index >= get_num_apps()) {
    // Bounce at boundary
    Animation *bounce_animation = create_bounce_animation(direction);
    animation_unschedule(s_previous_animation);
    animation_schedule(bounce_animation);
    s_previous_animation = bounce_animation;
    vibes_short_pulse();
    return;
  }

  // Update index and animate
  s_current_index = new_index;
  Animation *scroll_animation = create_scroll_animation(direction);
  animation_unschedule(s_previous_animation);
  animation_schedule(scroll_animation);
  s_previous_animation = scroll_animation;
}

// Button handlers
static void up_click_handler(ClickRecognizerRef recognizer, void *context) {
  scroll_to_index(s_current_index - 1, ScrollDirectionUp);
}

static void down_click_handler(ClickRecognizerRef recognizer, void *context) {
  scroll_to_index(s_current_index + 1, ScrollDirectionDown);
}

static void click_config_provider(void *context) {
  window_single_click_subscribe(BUTTON_ID_UP, up_click_handler);
  window_single_click_subscribe(BUTTON_ID_DOWN, down_click_handler);
}

// Loading window lifecycle
static void loading_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  // Create centered progress bar
  const int16_t progress_width = 100;
  const int16_t progress_height = 6;
  s_progress_layer = progress_layer_create(
    GRect((bounds.size.w - progress_width) / 2,
          bounds.size.h / 2 - progress_height / 2,
          progress_width,
          progress_height)
  );
  progress_layer_set_progress(s_progress_layer, 0);
  progress_layer_set_corner_radius(s_progress_layer, 2);

  // Set colors appropriately for color vs B&W displays
  // For B&W: grey background, black foreground
  // For color: light grey background, black foreground
#ifdef PBL_COLOR
  progress_layer_set_foreground_color(s_progress_layer, GColorBlack);
  progress_layer_set_background_color(s_progress_layer, GColorLightGray);
#else
  progress_layer_set_foreground_color(s_progress_layer, GColorBlack);
  progress_layer_set_background_color(s_progress_layer, GColorLightGray);
#endif

  layer_add_child(window_layer, s_progress_layer);
}

static void loading_window_unload(Window *window) {
  progress_layer_destroy(s_progress_layer);
  s_progress_layer = NULL;
}

// Main card window lifecycle
static void main_window_load(Window *window) {
  Layer *window_layer = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(window_layer);

  // Create status bar
  s_status_bar = status_bar_layer_create();
  status_bar_layer_set_colors(s_status_bar, GColorClear, GColorBlack);
  layer_add_child(window_layer, status_bar_layer_get_layer(s_status_bar));

  const int16_t margin = 8;
  const int16_t status_bar_height = 16;

  // Pagination layer (top right, in status bar area)
  s_pagination_layer = text_layer_create(GRect(bounds.size.w - 50 - 3, 0, 50, status_bar_height));
  text_layer_set_background_color(s_pagination_layer, GColorClear);
  text_layer_set_text_color(s_pagination_layer, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
  text_layer_set_font(s_pagination_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  text_layer_set_text_alignment(s_pagination_layer, GTextAlignmentRight);
  layer_add_child(window_layer, text_layer_get_layer(s_pagination_layer));

  // App name layer (bold, below status bar)
  s_name_layer = text_layer_create(GRect(margin, status_bar_height + 4, bounds.size.w - 2 * margin, 30));
  text_layer_set_background_color(s_name_layer, GColorClear);
  text_layer_set_text_color(s_name_layer, PBL_IF_COLOR_ELSE(GColorBlack, GColorBlack));
  text_layer_set_font(s_name_layer, fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD));
  text_layer_set_overflow_mode(s_name_layer, GTextOverflowModeTrailingEllipsis);
  layer_add_child(window_layer, text_layer_get_layer(s_name_layer));

  // Author layer (smaller, below name)
  s_author_layer = text_layer_create(GRect(margin, status_bar_height + 36, bounds.size.w - 2 * margin, 20));
  text_layer_set_background_color(s_author_layer, GColorClear);
  text_layer_set_text_color(s_author_layer, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
  text_layer_set_font(s_author_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  layer_add_child(window_layer, text_layer_get_layer(s_author_layer));

  // Description layer (main content)
  s_description_layer = text_layer_create(GRect(margin, status_bar_height + 64, bounds.size.w - 2 * margin, 60));
  text_layer_set_background_color(s_description_layer, GColorClear);
  text_layer_set_text_color(s_description_layer, PBL_IF_COLOR_ELSE(GColorBlack, GColorBlack));
  text_layer_set_font(s_description_layer, fonts_get_system_font(FONT_KEY_GOTHIC_18));
  text_layer_set_overflow_mode(s_description_layer, GTextOverflowModeWordWrap);
  layer_add_child(window_layer, text_layer_get_layer(s_description_layer));

  // Hearts layer (bottom)
  s_hearts_layer = text_layer_create(GRect(margin, bounds.size.h - 30, bounds.size.w - 2 * margin, 20));
  text_layer_set_background_color(s_hearts_layer, GColorClear);
  text_layer_set_text_color(s_hearts_layer, PBL_IF_COLOR_ELSE(GColorDarkGray, GColorBlack));
  text_layer_set_font(s_hearts_layer, fonts_get_system_font(FONT_KEY_GOTHIC_14));
  layer_add_child(window_layer, text_layer_get_layer(s_hearts_layer));

  // Initialize display with first app
  update_display();
}

static void main_window_unload(Window *window) {
  status_bar_layer_destroy(s_status_bar);
  text_layer_destroy(s_pagination_layer);
  text_layer_destroy(s_name_layer);
  text_layer_destroy(s_author_layer);
  text_layer_destroy(s_description_layer);
  text_layer_destroy(s_hearts_layer);
}

// AppMessage handlers
static void inbox_received_callback(DictionaryIterator *iterator, void *context) {
  // Check if this is the data complete signal
  Tuple *data_complete_tuple = dict_find(iterator, MESSAGE_KEY_DATA_COMPLETE);
  if (data_complete_tuple) {
    if (data_complete_tuple->value->int32 == 1) {
      // All apps received successfully
      APP_LOG(APP_LOG_LEVEL_INFO, "All apps received, transitioning to main window");
      s_data_loaded = true;

      // Create and push main window
      s_main_window = window_create();
      window_set_click_config_provider(s_main_window, click_config_provider);
      window_set_window_handlers(s_main_window, (WindowHandlers) {
        .load = main_window_load,
        .unload = main_window_unload,
      });
      window_stack_push(s_main_window, true);

      // Remove loading window
      window_stack_remove(s_loading_window, false);
    } else {
      // Error occurred - progress bar will remain at current state
      // Could add error indication via vibes_long_pulse() if desired
    }
    return;
  }

  // Read app data
  Tuple *index_tuple = dict_find(iterator, MESSAGE_KEY_APP_INDEX);
  Tuple *name_tuple = dict_find(iterator, MESSAGE_KEY_APP_NAME);
  Tuple *author_tuple = dict_find(iterator, MESSAGE_KEY_APP_AUTHOR);
  Tuple *description_tuple = dict_find(iterator, MESSAGE_KEY_APP_DESCRIPTION);
  Tuple *hearts_tuple = dict_find(iterator, MESSAGE_KEY_APP_HEARTS);
  Tuple *days_ago_tuple = dict_find(iterator, MESSAGE_KEY_APP_DAYS_AGO);

  if (index_tuple && name_tuple && author_tuple && description_tuple && hearts_tuple && days_ago_tuple) {
    int index = index_tuple->value->int32;

    if (index >= 0 && index < 10) {
      // Copy data into app array
      strncpy(s_apps[index].name, name_tuple->value->cstring, sizeof(s_apps[index].name) - 1);
      strncpy(s_apps[index].author, author_tuple->value->cstring, sizeof(s_apps[index].author) - 1);
      strncpy(s_apps[index].description, description_tuple->value->cstring, sizeof(s_apps[index].description) - 1);
      s_apps[index].hearts = hearts_tuple->value->int32;
      s_apps[index].days_ago = days_ago_tuple->value->int32;

      s_apps_received++;

      APP_LOG(APP_LOG_LEVEL_INFO, "Received app %d: %s", index, s_apps[index].name);

      // Update loading percentage
      int percentage = (s_apps_received * 100) / get_num_apps();
      update_loading_progress(percentage);
    }
  }
}

static void inbox_dropped_callback(AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Message dropped: %d", (int)reason);
}

static void outbox_failed_callback(DictionaryIterator *iterator, AppMessageResult reason, void *context) {
  APP_LOG(APP_LOG_LEVEL_ERROR, "Outbox send failed: %d", (int)reason);
}

static void outbox_sent_callback(DictionaryIterator *iterator, void *context) {
  APP_LOG(APP_LOG_LEVEL_INFO, "Outbox send success");
}

// AppGlance callback to update the app glance
static void app_glance_update_callback(AppGlanceReloadSession *session, size_t limit, void *context) {
  if (limit < 1) return;

  int new_apps_count = count_new_apps_this_week();

  // Format the message
  static char message[64];
  if (new_apps_count == 1) {
    snprintf(message, sizeof(message), "1 new app this week");
  } else {
    snprintf(message, sizeof(message), "%d new apps this week", new_apps_count);
  }

  // Create the AppGlance slice
  const AppGlanceSlice slice = (AppGlanceSlice) {
    .layout = {
      .icon = APP_GLANCE_SLICE_DEFAULT_ICON,
      .subtitle_template_string = message
    },
    .expiration_time = get_sunday_expiration()
  };

  const AppGlanceResult result = app_glance_add_slice(session, slice);
  if (result != APP_GLANCE_RESULT_SUCCESS) {
    APP_LOG(APP_LOG_LEVEL_ERROR, "AppGlance Error: %d", result);
  }
}

static void init(void) {
  // Clear existing AppGlance slices
  app_glance_reload(NULL, NULL);

  // Initialize AppMessage
  app_message_register_inbox_received(inbox_received_callback);
  app_message_register_inbox_dropped(inbox_dropped_callback);
  app_message_register_outbox_failed(outbox_failed_callback);
  app_message_register_outbox_sent(outbox_sent_callback);

  // Open AppMessage with appropriate buffer sizes
  const int inbox_size = 256;
  const int outbox_size = 64;
  app_message_open(inbox_size, outbox_size);

  // Create and show loading window
  s_loading_window = window_create();
  window_set_window_handlers(s_loading_window, (WindowHandlers) {
    .load = loading_window_load,
    .unload = loading_window_unload,
  });
  window_stack_push(s_loading_window, true);

  APP_LOG(APP_LOG_LEVEL_INFO, "Loading window shown, waiting for data from JavaScript");
}

static void deinit(void) {
  // Update AppGlance with new apps count only if data was successfully loaded
  if (s_data_loaded) {
    app_glance_reload(app_glance_update_callback, NULL);
  }

  if (s_main_window) {
    window_destroy(s_main_window);
  }
  if (s_loading_window) {
    window_destroy(s_loading_window);
  }
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}

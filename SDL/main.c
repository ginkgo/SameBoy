#include <stdbool.h>
#include <stdio.h>
#include <signal.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <OpenDialog/open_dialog.h>
#include <SDL.h>
#include <Core/gb.h>
#include "utils.h"
#include "gui.h"
#include "shader.h"
#include "audio/audio.h"
#include "console.h"

#include <zmq.h>
#include "traceboy.pb-c.h"

#ifndef _WIN32
#include <fcntl.h>
#else
#include <Windows.h>
#endif

static bool stop_on_start = false;
GB_gameboy_t gb;
static bool paused = false;
static uint32_t pixel_buffer_1[256 * 224], pixel_buffer_2[256 * 224];
static uint32_t *active_pixel_buffer = pixel_buffer_1, *previous_pixel_buffer = pixel_buffer_2;
static bool underclock_down = false, rewind_down = false, do_rewind = false, rewind_paused = false, turbo_down = false;
static double clock_mutliplier = 1.0;
static GB_key_mask_t key_mask = 0;
static bool vblank_just_occured = false;

void update_key_mask(GB_key_t index, bool pressed)
{
	key_mask &= ~(1<<index);
	key_mask |= pressed ? (1<<index) : 0;
}

char *filename = NULL;
static typeof(free) *free_function = NULL;
static char *battery_save_path_ptr = NULL;
static SDL_GLContext gl_context = NULL;
static bool console_supported = false;

bool uses_gl(void)
{
    return gl_context;
}

void set_filename(const char *new_filename, typeof(free) *new_free_function)
{
    if (filename && free_function) {
        free_function(filename);
    }
    filename = (char *) new_filename;
    free_function = new_free_function;
    GB_rewind_reset(&gb);
}

static char *completer(const char *substring, uintptr_t *context)
{
    if (!GB_is_inited(&gb)) return NULL;
    char *temp = strdup(substring);
    char *ret = GB_debugger_complete_substring(&gb, temp, context);
    free(temp);
    return ret;
}

static void log_callback(GB_gameboy_t *gb, const char *string, GB_log_attributes attributes)
{
    CON_attributes_t con_attributes = {0,};
    con_attributes.bold = attributes & GB_LOG_BOLD;
    con_attributes.underline = attributes & GB_LOG_UNDERLINE;
    if (attributes & GB_LOG_DASHED_UNDERLINE) {
        while (*string) {
            con_attributes.underline ^= true;
            CON_attributed_printf("%c", &con_attributes, *string);
            string++;
        }
    }
    else {
        CON_attributed_print(string, &con_attributes);
    }
}

static void handle_eof(void)
{
    CON_set_async_prompt("");
    char *line = CON_readline("Quit? [y]/n > ");
    if (line[0] == 'n' || line[0] == 'N') {
        free(line);
        CON_set_async_prompt("> ");
    }
    else {
        exit(0);
    }
}

static char *input_callback(GB_gameboy_t *gb)
{
retry: {
    char *ret = CON_readline("Stopped> ");
    if (strcmp(ret, CON_EOF) == 0) {
        handle_eof();
        free(ret);
        goto retry;
    }
    else {
        CON_attributes_t attributes = {.bold = true};
        CON_attributed_printf("> %s\n", &attributes, ret);
    }
    return ret;
}
}

static char *asyc_input_callback(GB_gameboy_t *gb)
{
retry: {
    char *ret = CON_readline_async();
    if (ret && strcmp(ret, CON_EOF) == 0) {
        handle_eof();
        free(ret);
        goto retry;
    }
    else if (ret) {
        CON_attributes_t attributes = {.bold = true};
        CON_attributed_printf("> %s\n", &attributes, ret);
    }
    return ret;
}
}


static char *captured_log = NULL;

static void log_capture_callback(GB_gameboy_t *gb, const char *string, GB_log_attributes attributes)
{
    size_t current_len = strlen(captured_log);
    size_t len_to_add = strlen(string);
    captured_log = realloc(captured_log, current_len + len_to_add + 1);
    memcpy(captured_log + current_len, string, len_to_add);
    captured_log[current_len + len_to_add] = 0;
}

static void start_capturing_logs(void)
{
    if (captured_log != NULL) {
        free(captured_log);
    }
    captured_log = malloc(1);
    captured_log[0] = 0;
    GB_set_log_callback(&gb, log_capture_callback);
}

static const char *end_capturing_logs(bool show_popup, bool should_exit, uint32_t popup_flags, const char *title)
{
    GB_set_log_callback(&gb, console_supported? log_callback : NULL);
    if (captured_log[0] == 0) {
        free(captured_log);
        captured_log = NULL;
    }
    else {
        if (show_popup) {
            SDL_ShowSimpleMessageBox(popup_flags, title, captured_log, window);
        }
        if (should_exit) {
            exit(1);
        }
    }
    return captured_log;
}

static void update_palette(void)
{
    GB_set_palette(&gb, current_dmg_palette());
}

static void screen_size_changed(void)
{
    SDL_DestroyTexture(texture);
    texture = SDL_CreateTexture(renderer, SDL_GetWindowPixelFormat(window), SDL_TEXTUREACCESS_STREAMING,
                                GB_get_screen_width(&gb), GB_get_screen_height(&gb));
    
    SDL_SetWindowMinimumSize(window, GB_get_screen_width(&gb), GB_get_screen_height(&gb));
    
    update_viewport();
}

static void open_menu(void)
{
    bool audio_playing = GB_audio_is_playing();
    if (audio_playing) {
        GB_audio_set_paused(true);
    }
    size_t previous_width = GB_get_screen_width(&gb);
    run_gui(true);
    SDL_ShowCursor(SDL_DISABLE);
    if (audio_playing) {
        GB_audio_set_paused(false);
    }
    GB_set_color_correction_mode(&gb, configuration.color_correction_mode);
    GB_set_light_temperature(&gb, (configuration.color_temperature - 10.0) / 10.0);
    GB_set_interference_volume(&gb, configuration.interference_volume / 100.0);
    GB_set_border_mode(&gb, configuration.border_mode);
    update_palette();
    GB_set_highpass_filter_mode(&gb, configuration.highpass_mode);
    GB_set_rewind_length(&gb, configuration.rewind_length);
    GB_set_rtc_mode(&gb, configuration.rtc_mode);
    if (previous_width != GB_get_screen_width(&gb)) {
        screen_size_changed();
    }
}

static void handle_events(GB_gameboy_t *gb)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_DISPLAYEVENT:
                update_swap_interval();
                break;
            case SDL_QUIT:
                pending_command = GB_SDL_QUIT_COMMAND;
                break;
                
            case SDL_DROPFILE: {
                if (GB_is_save_state(event.drop.file)) {
                    dropped_state_file = event.drop.file;
                    pending_command = GB_SDL_LOAD_STATE_FROM_FILE_COMMAND;
                }
                else {
                    set_filename(event.drop.file, SDL_free);
                    pending_command = GB_SDL_NEW_FILE_COMMAND;
                }
                break;
            }
                
            case SDL_WINDOWEVENT: {
                if (event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    update_viewport();
                }
                if (event.window.type == SDL_WINDOWEVENT_MOVED
#if SDL_COMPILEDVERSION > 2018
                    || event.window.type == SDL_WINDOWEVENT_DISPLAY_CHANGED
#endif
                    ) {
                    update_swap_interval();
                }
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP: {
                if (GB_has_accelerometer(gb) && configuration.allow_mouse_controls) {
					update_key_mask(GB_KEY_A, event.type == SDL_MOUSEBUTTONDOWN);
                }
                break;
            }
                
            case SDL_MOUSEMOTION: {
                if (GB_has_accelerometer(gb) && configuration.allow_mouse_controls) {
                    signed x = event.motion.x;
                    signed y = event.motion.y;
                    convert_mouse_coordinates(&x, &y);
                    x = SDL_max(SDL_min(x, 160), 0);
                    y = SDL_max(SDL_min(y, 144), 0);
                    GB_set_accelerometer_values(gb, (x - 80) / -80.0, (y - 72) / -72.0);
                }
                break;
            }
                
            case SDL_JOYDEVICEREMOVED:
                if (joystick && event.jdevice.which == SDL_JoystickInstanceID(joystick)) {
                    SDL_JoystickClose(joystick);
                    joystick = NULL;
                }
            case SDL_JOYDEVICEADDED:
                connect_joypad();
                break;
                
            case SDL_JOYBUTTONUP:
            case SDL_JOYBUTTONDOWN: {
                joypad_button_t button = get_joypad_button(event.jbutton.button);
                if ((GB_key_t) button < GB_KEY_MAX) {
                    update_key_mask((GB_key_t) button, event.type == SDL_JOYBUTTONDOWN);
                }
                else if (button == JOYPAD_BUTTON_TURBO) {
                    GB_audio_clear_queue();
                    turbo_down = event.type == SDL_JOYBUTTONDOWN;
                    GB_set_turbo_mode(gb, turbo_down, turbo_down && rewind_down);
                }
                else if (button == JOYPAD_BUTTON_SLOW_MOTION) {
                    underclock_down = event.type == SDL_JOYBUTTONDOWN;
                }
                else if (button == JOYPAD_BUTTON_REWIND) {
                    rewind_down = event.type == SDL_JOYBUTTONDOWN;
                    if (event.type == SDL_JOYBUTTONUP) {
                        rewind_paused = false;
                    }
                    GB_set_turbo_mode(gb, turbo_down, turbo_down && rewind_down);
                }
                else if (button == JOYPAD_BUTTON_MENU && event.type == SDL_JOYBUTTONDOWN) {
                    open_menu();
                }
                else if ((button == JOYPAD_BUTTON_HOTKEY_1 || button == JOYPAD_BUTTON_HOTKEY_2) && event.type == SDL_JOYBUTTONDOWN) {
                    hotkey_action_t action = configuration.hotkey_actions[button - JOYPAD_BUTTON_HOTKEY_1];
                    switch (action) {
                        case HOTKEY_NONE:
                            break;
                        case HOTKEY_PAUSE:
                            paused = !paused;
                            break;
                        case HOTKEY_MUTE:
                            GB_audio_set_paused(GB_audio_is_playing());
                            break;
                        case HOTKEY_RESET:
                            pending_command = GB_SDL_RESET_COMMAND;
                            break;
                        case HOTKEY_QUIT:
                            pending_command = GB_SDL_QUIT_COMMAND;
                            break;
                        default:
                            command_parameter = (action - HOTKEY_SAVE_STATE_1) / 2 + 1;
                            pending_command = ((action - HOTKEY_SAVE_STATE_1) % 2)? GB_SDL_LOAD_STATE_COMMAND:GB_SDL_SAVE_STATE_COMMAND;
                            break;
                        case HOTKEY_SAVE_STATE_10:
                            command_parameter = 0;
                            pending_command = GB_SDL_SAVE_STATE_COMMAND;
                            break;
                        case HOTKEY_LOAD_STATE_10:
                            command_parameter = 0;
                            pending_command = GB_SDL_LOAD_STATE_COMMAND;
                            break;
                    }
                }
            }
                break;
                
            case SDL_JOYAXISMOTION: {
                static bool axis_active[2] = {false, false};
                static double accel_values[2] = {0, 0};
                joypad_axis_t axis = get_joypad_axis(event.jaxis.axis);
                if (axis == JOYPAD_AXISES_X) {
                    if (GB_has_accelerometer(gb)) {
                        accel_values[0] = event.jaxis.value / (double)32768.0;
                        GB_set_accelerometer_values(gb, -accel_values[0], -accel_values[1]);
                    }
                    else if (event.jaxis.value > JOYSTICK_HIGH) {
                        axis_active[0] = true;
                        update_key_mask(GB_KEY_RIGHT, true);
                        update_key_mask(GB_KEY_LEFT, false);
                    }
                    else if (event.jaxis.value < -JOYSTICK_HIGH) {
                        axis_active[0] = true;
                        update_key_mask(GB_KEY_RIGHT, false);
                        update_key_mask(GB_KEY_LEFT, true);
                    }
                    else if (axis_active[0] && event.jaxis.value < JOYSTICK_LOW && event.jaxis.value > -JOYSTICK_LOW) {
                        axis_active[0] = false;
                        update_key_mask(GB_KEY_RIGHT, false);
                        update_key_mask(GB_KEY_LEFT, false);
                    }
                }
                else if (axis == JOYPAD_AXISES_Y) {
                    if (GB_has_accelerometer(gb)) {
                        accel_values[1] = event.jaxis.value / (double)32768.0;
                        GB_set_accelerometer_values(gb, -accel_values[0], -accel_values[1]);
                    }
                    else if (event.jaxis.value > JOYSTICK_HIGH) {
                        axis_active[1] = true;
                        update_key_mask(GB_KEY_DOWN, true);
                        update_key_mask(GB_KEY_UP, false);
                    }
                    else if (event.jaxis.value < -JOYSTICK_HIGH) {
                        axis_active[1] = true;
                        update_key_mask(GB_KEY_DOWN, false);
                        update_key_mask(GB_KEY_UP, true);
                    }
                    else if (axis_active[1] && event.jaxis.value < JOYSTICK_LOW && event.jaxis.value > -JOYSTICK_LOW) {
                        axis_active[1] = false;
                        update_key_mask(GB_KEY_DOWN, false);
                        update_key_mask(GB_KEY_UP, false);
                    }
                }
            }
                break;
                
            case SDL_JOYHATMOTION: {
                uint8_t value = event.jhat.value;
                int8_t updown =
                value == SDL_HAT_LEFTUP || value == SDL_HAT_UP || value == SDL_HAT_RIGHTUP ? -1 : (value == SDL_HAT_LEFTDOWN || value == SDL_HAT_DOWN || value == SDL_HAT_RIGHTDOWN ? 1 : 0);
                int8_t leftright =
                value == SDL_HAT_LEFTUP || value == SDL_HAT_LEFT || value == SDL_HAT_LEFTDOWN ? -1 : (value == SDL_HAT_RIGHTUP || value == SDL_HAT_RIGHT || value == SDL_HAT_RIGHTDOWN ? 1 : 0);
                
                update_key_mask(GB_KEY_LEFT, leftright == -1);
                update_key_mask(GB_KEY_RIGHT, leftright == 1);
                update_key_mask(GB_KEY_UP, updown == -1);
                update_key_mask(GB_KEY_DOWN, updown == 1);
                break;
            };
                
            case SDL_KEYDOWN:
                switch (event_hotkey_code(&event)) {
                    case SDL_SCANCODE_ESCAPE: {
                        open_menu();
                        break;
                    }
                    case SDL_SCANCODE_C:
                        if (event.type == SDL_KEYDOWN && (event.key.keysym.mod & KMOD_CTRL)) {
                            CON_print("^C\a\n");
                            GB_debugger_break(gb);
                        }
                        break;
                        
                    case SDL_SCANCODE_R:
                        if (event.key.keysym.mod & MODIFIER) {
                            pending_command = GB_SDL_RESET_COMMAND;
                        }
                        break;
                        
                    case SDL_SCANCODE_O: {
                        if (event.key.keysym.mod & MODIFIER) {
                            char *filename = do_open_rom_dialog();
                            if (filename) {
                                set_filename(filename, free);
                                pending_command = GB_SDL_NEW_FILE_COMMAND;
                            }
                        }
                        break;
                    }
                        
                    case SDL_SCANCODE_P:
                        if (event.key.keysym.mod & MODIFIER) {
                            paused = !paused;
                        }
                        break;
                    case SDL_SCANCODE_M:
                        if (event.key.keysym.mod & MODIFIER) {
#ifdef __APPLE__
                            // Can't override CMD+M (Minimize) in SDL
                            if (!(event.key.keysym.mod & KMOD_SHIFT)) {
                                break;
                            }
#endif
                            GB_audio_set_paused(GB_audio_is_playing());
                        }
                        break;
                        
                    case SDL_SCANCODE_F:
                        if (event.key.keysym.mod & MODIFIER) {
                            if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP)) {
                                SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
                            }
                            else {
                                SDL_SetWindowFullscreen(window, 0);
                            }
                            update_swap_interval();
                            update_viewport();
                        }
                        break;
                        
                    default:
                        /* Save states */
                        if (event.key.keysym.scancode >= SDL_SCANCODE_1 && event.key.keysym.scancode <= SDL_SCANCODE_0) {
                            if (event.key.keysym.mod & MODIFIER) {
                                command_parameter = (event.key.keysym.scancode - SDL_SCANCODE_1 + 1) % 10;
                                
                                if (event.key.keysym.mod & KMOD_SHIFT) {
                                    pending_command = GB_SDL_LOAD_STATE_COMMAND;
                                }
                                else {
                                    pending_command = GB_SDL_SAVE_STATE_COMMAND;
                                }
                            }
                            else if ((event.key.keysym.mod & KMOD_ALT) && event.key.keysym.scancode <= SDL_SCANCODE_4) {
                                GB_channel_t channel = event.key.keysym.scancode - SDL_SCANCODE_1;
                                bool state = !GB_is_channel_muted(gb, channel);
                                
                                GB_set_channel_muted(gb, channel, state);
                                
                                static char message[18];
                                sprintf(message, "Channel %d %smuted", channel + 1, state? "" : "un");
                                show_osd_text(message);
                            }
                        }
                        break;
                }
            case SDL_KEYUP: // Fallthrough
                if (event.key.keysym.scancode == configuration.keys[8]) {
                    turbo_down = event.type == SDL_KEYDOWN;
                    GB_audio_clear_queue();
                    GB_set_turbo_mode(gb, turbo_down, turbo_down && rewind_down);
                }
                else if (event.key.keysym.scancode == configuration.keys_2[0]) {
                    rewind_down = event.type == SDL_KEYDOWN;
                    if (event.type == SDL_KEYUP) {
                        rewind_paused = false;
                    }
                    GB_set_turbo_mode(gb, turbo_down, turbo_down && rewind_down);
                }
                else if (event.key.keysym.scancode == configuration.keys_2[1]) {
                    underclock_down = event.type == SDL_KEYDOWN;
                }
                else {
                    for (unsigned i = 0; i < GB_KEY_MAX; i++) {
                        if (event.key.keysym.scancode == configuration.keys[i]) {
                            update_key_mask(i, event.type == SDL_KEYDOWN);
                        }
                    }
                }
                break;
            default:
                break;
        }
    }
}

static uint32_t rgb_encode(GB_gameboy_t *gb, uint8_t r, uint8_t g, uint8_t b)
{
    return SDL_MapRGB(pixel_format, r, g, b);
}

static uint32_t calc_crc32(size_t size, const uint8_t *byte)
{
    static const uint32_t table[] = {
        0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F,
        0xE963A535, 0x9E6495A3, 0x0EDB8832, 0x79DCB8A4, 0xE0D5E91E, 0x97D2D988,
        0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
        0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7,
        0x136C9856, 0x646BA8C0, 0xFD62F97A, 0x8A65C9EC, 0x14015C4F, 0x63066CD9,
        0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
        0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C,
        0xDBBBC9D6, 0xACBCF940, 0x32D86CE3, 0x45DF5C75, 0xDCD60DCF, 0xABD13D59,
        0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
        0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924,
        0x2F6F7C87, 0x58684C11, 0xC1611DAB, 0xB6662D3D, 0x76DC4190, 0x01DB7106,
        0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
        0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D,
        0x91646C97, 0xE6635C01, 0x6B6B51F4, 0x1C6C6162, 0x856530D8, 0xF262004E,
        0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
        0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65,
        0x4DB26158, 0x3AB551CE, 0xA3BC0074, 0xD4BB30E2, 0x4ADFA541, 0x3DD895D7,
        0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
        0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C, 0x270241AA,
        0xBE0B1010, 0xC90C2086, 0x5768B525, 0x206F85B3, 0xB966D409, 0xCE61E49F,
        0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
        0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A,
        0xEAD54739, 0x9DD277AF, 0x04DB2615, 0x73DC1683, 0xE3630B12, 0x94643B84,
        0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
        0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB,
        0x196C3671, 0x6E6B06E7, 0xFED41B76, 0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC,
        0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
        0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B,
        0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6, 0x41047A60, 0xDF60EFC3, 0xA867DF55,
        0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
        0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28,
        0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7, 0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D,
        0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
        0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38,
        0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7, 0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242,
        0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
        0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69,
        0x616BFFD3, 0x166CCF45, 0xA00AE278, 0xD70DD2EE, 0x4E048354, 0x3903B3C2,
        0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
        0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9,
        0xBDBDF21C, 0xCABAC28A, 0x53B39330, 0x24B4A3A6, 0xBAD03605, 0xCDD70693,
        0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
        0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D
    };

    uint32_t ret = 0xFFFFFFFF;
    while (size--) {
        ret = table[(ret ^ *byte++) & 0xFF] ^ (ret >> 8);
    }
    return ~ret;
}

static void vblank(GB_gameboy_t *gb, GB_vblank_type_t type)
{
	if (type == GB_VBLANK_TYPE_NORMAL_FRAME)
	{
		vblank_just_occured = true;
	}

    if (underclock_down && clock_mutliplier > 0.5) {
        clock_mutliplier -= 1.0/16;
        GB_set_clock_multiplier(gb, clock_mutliplier);
    }
    else if (!underclock_down && clock_mutliplier < 1.0) {
        clock_mutliplier += 1.0/16;
        GB_set_clock_multiplier(gb, clock_mutliplier);
    }
    
    if (turbo_down) {
        show_osd_text("Fast forward...");
    }
    else if (underclock_down) {
        show_osd_text("Slow motion...");
    }
    else if (rewind_down) {
        show_osd_text("Rewinding...");
    }
    
    if (osd_countdown && configuration.osd) {
        unsigned width = GB_get_screen_width(gb);
        unsigned height = GB_get_screen_height(gb);
        draw_text(active_pixel_buffer,
                  width, height, 8, height - 8 - osd_text_lines * 12, osd_text,
                  rgb_encode(gb, 255, 255, 255), rgb_encode(gb, 0, 0, 0),
                  true);
        osd_countdown--;
    }
    if (type != GB_VBLANK_TYPE_REPEAT) {
        if (configuration.blending_mode) {
            render_texture(active_pixel_buffer, previous_pixel_buffer);
            uint32_t *temp = active_pixel_buffer;
            active_pixel_buffer = previous_pixel_buffer;
            previous_pixel_buffer = temp;
            GB_set_pixels_output(gb, active_pixel_buffer);
        }
        else {
            render_texture(active_pixel_buffer, NULL);
        }
    }
    do_rewind = rewind_down;

    handle_events(gb);
}

static void rumble(GB_gameboy_t *gb, double amp)
{
    SDL_HapticRumblePlay(haptic, amp, 250);
}

static void debugger_interrupt(int ignore)
{
    if (!GB_is_inited(&gb)) exit(0);
    /* ^C twice to exit */
    if (GB_debugger_is_stopped(&gb)) {
        GB_save_battery(&gb, battery_save_path_ptr);
        exit(0);
    }
    if (console_supported) {
        CON_print("^C\n");
    }
    GB_debugger_break(&gb);
}

#ifndef _WIN32
static void debugger_reset(int ignore)
{
    pending_command = GB_SDL_RESET_COMMAND;
}
#endif

static void gb_audio_callback(GB_gameboy_t *gb, GB_sample_t *sample)
{    
    if (turbo_down) {
        static unsigned skip = 0;
        skip++;
        if (skip == GB_audio_get_frequency() / 8) {
            skip = 0;
        }
        if (skip > GB_audio_get_frequency() / 16) {
            return;
        }
    }
    
    if (GB_audio_get_queue_length() > GB_audio_get_frequency() / 8) { // Maximum lag of 0.125s
        return;
    }
    
    if (configuration.volume != 100) {
        sample->left = sample->left * configuration.volume / 100;
        sample->right = sample->right * configuration.volume / 100;
    }
    
    GB_audio_queue_sample(sample);
    
}
    
static bool doing_hot_swap = false;
static bool handle_pending_command(void)
{

    switch (pending_command) {
        case GB_SDL_LOAD_STATE_COMMAND:
        case GB_SDL_SAVE_STATE_COMMAND: {
            char save_path[strlen(filename) + 5];
            char save_extension[] = ".s0";
            save_extension[2] += command_parameter;
            replace_extension(filename, strlen(filename), save_path, save_extension);
            
            start_capturing_logs();
            bool success;
            if (pending_command == GB_SDL_LOAD_STATE_COMMAND) {
                int result = GB_load_state(&gb, save_path);
                if (result == ENOENT) {
                    char save_extension[] = ".sn0";
                    save_extension[3] += command_parameter;
                    replace_extension(filename, strlen(filename), save_path, save_extension);
                    start_capturing_logs();
                    result = GB_load_state(&gb, save_path);
                }
                success = result == 0;
            }
            else {
                success = GB_save_state(&gb, save_path) == 0;
            }
            end_capturing_logs(true,
                               false,
                               success? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_ERROR,
                               success? "Notice" : "Error");
            if (success) {
                show_osd_text(pending_command == GB_SDL_LOAD_STATE_COMMAND? "State loaded" : "State saved");
            }
            return false;
        }
    
        case GB_SDL_LOAD_STATE_FROM_FILE_COMMAND:
            start_capturing_logs();
            bool success = GB_load_state(&gb, dropped_state_file) == 0;
            end_capturing_logs(true,
                               false,
                               success? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_ERROR,
                               success? "Notice" : "Error");
            SDL_free(dropped_state_file);
            if (success) {
                show_osd_text("State loaded");
            }
            return false;
            
        case GB_SDL_NO_COMMAND:
            return false;
            
        case GB_SDL_CART_SWAP_COMMAND:
            doing_hot_swap = true;
        case GB_SDL_RESET_COMMAND:
        case GB_SDL_NEW_FILE_COMMAND:
            GB_save_battery(&gb, battery_save_path_ptr);
            return true;
            
        case GB_SDL_QUIT_COMMAND:
            GB_save_battery(&gb, battery_save_path_ptr);
            exit(0);
    }
    return false;
}

static void load_boot_rom(GB_gameboy_t *gb, GB_boot_rom_t type)
{
    static const char *const names[] = {
        [GB_BOOT_ROM_DMG_0] = "dmg0_boot.bin",
        [GB_BOOT_ROM_DMG] = "dmg_boot.bin",
        [GB_BOOT_ROM_MGB] = "mgb_boot.bin",
        [GB_BOOT_ROM_SGB] = "sgb_boot.bin",
        [GB_BOOT_ROM_SGB2] = "sgb2_boot.bin",
        [GB_BOOT_ROM_CGB_0] = "cgb0_boot.bin",
        [GB_BOOT_ROM_CGB] = "cgb_boot.bin",
        [GB_BOOT_ROM_CGB_E] = "cgbE_boot.bin",
        [GB_BOOT_ROM_AGB_0] = "agb0_boot.bin",
        [GB_BOOT_ROM_AGB] = "agb_boot.bin",
    };
    bool use_built_in = true;

	printf("Loading boot rom %s...\n", names[type]);

    if (configuration.bootrom_path[0]) {
        static char path[PATH_MAX + 1];
        snprintf(path, sizeof(path), "%s/%s", configuration.bootrom_path, names[type]);
        use_built_in = GB_load_boot_rom(gb, path);
    }
    if (use_built_in) {
        start_capturing_logs();
        if (GB_load_boot_rom(gb, resource_path(names[type]))) {
            if (type == GB_BOOT_ROM_CGB_E) {
                load_boot_rom(gb, GB_BOOT_ROM_CGB);
                return;
            }
            if (type == GB_BOOT_ROM_AGB_0) {
                load_boot_rom(gb, GB_BOOT_ROM_AGB);
                return;
            }
        }
        end_capturing_logs(true, false, SDL_MESSAGEBOX_ERROR, "Error");
    }
}

static bool is_path_writeable(const char *path)
{
    if (!access(path, W_OK)) return true;
    int fd = creat(path, 0644);
    if (fd == -1) return false;
    close(fd);
    unlink(path);
    return true;
}

static bool has_active_trace_packet = false;

static void debugger_reload_callback(GB_gameboy_t *gb)
{
    size_t path_length = strlen(filename);
    char extension[4] = {0,};
    if (path_length > 4) {
        if (filename[path_length - 4] == '.') {
            extension[0] = tolower((unsigned char)filename[path_length - 3]);
            extension[1] = tolower((unsigned char)filename[path_length - 2]);
            extension[2] = tolower((unsigned char)filename[path_length - 1]);
        }
    }
    if (strcmp(extension, "isx") == 0) {
        GB_load_isx(gb, filename);
    }
    else {
        GB_load_rom(gb, filename);
    }
    
	has_active_trace_packet = false;

    GB_load_battery(gb, battery_save_path_ptr);
    
    GB_debugger_clear_symbols(gb);
    GB_debugger_load_symbol_file(gb, resource_path("registers.sym"));
    
    char symbols_path[path_length + 5];
    replace_extension(filename, path_length, symbols_path, ".sym");
    GB_debugger_load_symbol_file(gb, symbols_path);
    
    GB_reset(gb);
}

static void issue_trace_packet()
{
	static uint8_t inputs[20*60];
	static int frame_count = -1;
	static uint8_t *start_state = NULL;
	static size_t start_state_size;

	static void *context = NULL;
	static void *requester = NULL;

	if (!has_active_trace_packet)
	{
		if (!context)
		{
			context = zmq_ctx_new();
			requester = zmq_socket(context, ZMQ_PUSH);
			zmq_connect (requester, "tcp://localhost:1989");
		}

		if (start_state)
		{
			free(start_state);
		}

		start_state_size = GB_get_save_state_size(&gb);
		start_state = malloc(start_state_size);
		GB_save_state_to_buffer(&gb, start_state);

		frame_count = 0;
		has_active_trace_packet = true;
	}

	inputs[frame_count++] = key_mask;

	if (frame_count == sizeof(inputs))
	{
		size_t end_state_size = GB_get_save_state_size(&gb);
		uint8_t *end_state = malloc(end_state_size);
		GB_save_state_to_buffer(&gb, end_state);

		TracePacket trace_packet;
		trace_packet__init(&trace_packet);
		trace_packet.game_rom_crc32 = GB_get_rom_crc32(&gb);;
		trace_packet.start_state.len = start_state_size;
		trace_packet.start_state.data = start_state;
		trace_packet.user_inputs.len = frame_count-1;
		trace_packet.user_inputs.data = inputs;
		trace_packet.end_state_crc32 = calc_crc32(end_state_size, end_state);

		size_t packed_size = trace_packet__get_packed_size(&trace_packet);
		uint8_t *packed_msg = malloc(packed_size);
		assert(packed_msg);
		trace_packet__pack(&trace_packet, packed_msg);

		zmq_send (requester, packed_msg, packed_size, ZMQ_DONTWAIT);

		free(packed_msg);
		free(start_state);
		start_state = end_state;
		start_state_size = end_state_size;

		frame_count = 1;
		inputs[0] = key_mask;
	}
}

static void run(void)
{
    SDL_ShowCursor(SDL_DISABLE);
    GB_model_t model;
    pending_command = GB_SDL_NO_COMMAND;
restart:

    model = (GB_model_t [])
    {
        [MODEL_DMG] = GB_MODEL_DMG_B,
        [MODEL_CGB] = GB_MODEL_CGB_0 + configuration.cgb_revision,
        [MODEL_AGB] = configuration.agb_revision,
        [MODEL_MGB] = GB_MODEL_MGB,
        [MODEL_SGB] = (GB_model_t [])
        {
            [SGB_NTSC] = GB_MODEL_SGB_NTSC,
            [SGB_PAL] = GB_MODEL_SGB_PAL,
            [SGB_2] = GB_MODEL_SGB2,
        }[configuration.sgb_revision],
    }[configuration.model];
    
    if (GB_is_inited(&gb)) {
        if (doing_hot_swap) {
            doing_hot_swap = false;
        }
        else {
            GB_switch_model_and_reset(&gb, model);
        }
    }
    else {
        GB_init(&gb, model);
		GB_random_set_enabled(false);
		GB_set_emulate_joypad_bouncing(&gb, false);
        
        GB_set_boot_rom_load_callback(&gb, load_boot_rom);
        GB_set_vblank_callback(&gb, (GB_vblank_callback_t) vblank);
        GB_set_pixels_output(&gb, active_pixel_buffer);
        GB_set_rgb_encode_callback(&gb, rgb_encode);
        GB_set_rumble_callback(&gb, rumble);
        GB_set_rumble_mode(&gb, configuration.rumble_mode);
        GB_set_sample_rate(&gb, GB_audio_get_frequency());
        GB_set_color_correction_mode(&gb, configuration.color_correction_mode);
        GB_set_light_temperature(&gb, (configuration.color_temperature - 10.0) / 10.0);
        GB_set_interference_volume(&gb, configuration.interference_volume / 100.0);
        update_palette();
        if ((unsigned)configuration.border_mode <= GB_BORDER_ALWAYS) {
            GB_set_border_mode(&gb, configuration.border_mode);
        }
        GB_set_highpass_filter_mode(&gb, configuration.highpass_mode);
        GB_set_rewind_length(&gb, configuration.rewind_length);
        GB_set_rtc_mode(&gb, configuration.rtc_mode);
        GB_set_update_input_hint_callback(&gb, handle_events);
        GB_apu_set_sample_callback(&gb, gb_audio_callback);
        
        if (console_supported) {
            CON_set_async_prompt("> ");
            GB_set_log_callback(&gb, log_callback);
            GB_set_input_callback(&gb, input_callback);
            GB_set_async_input_callback(&gb, asyc_input_callback);
        }
        
        GB_set_debugger_reload_callback(&gb, debugger_reload_callback);
    }
    if (stop_on_start) {
        stop_on_start = false;
        GB_debugger_break(&gb);
    }

    bool error = false;
    GB_debugger_clear_symbols(&gb);
    start_capturing_logs();
    size_t path_length = strlen(filename);
    char extension[4] = {0,};
    if (path_length > 4) {
        if (filename[path_length - 4] == '.') {
            extension[0] = tolower((unsigned char)filename[path_length - 3]);
            extension[1] = tolower((unsigned char)filename[path_length - 2]);
            extension[2] = tolower((unsigned char)filename[path_length - 1]);
        }
    }
    if (strcmp(extension, "isx") == 0) {
        error = GB_load_isx(&gb, filename);
        /* Configure battery */
        char battery_save_path[path_length + 5]; /* At the worst case, size is strlen(path) + 4 bytes for .sav + NULL */
        replace_extension(filename, path_length, battery_save_path, ".ram");
        battery_save_path_ptr = battery_save_path;
        GB_load_battery(&gb, battery_save_path);
    }
    else {
        GB_load_rom(&gb, filename);
    }
    
    /* Configure battery */
    char battery_save_path[path_length + 5]; /* At the worst case, size is strlen(path) + 4 bytes for .sav + NULL */
    replace_extension(filename, path_length, battery_save_path, ".sav");
    battery_save_path_ptr = battery_save_path;
    GB_load_battery(&gb, battery_save_path);
    if (GB_save_battery_size(&gb)) {
        if (!is_path_writeable(battery_save_path)) {
            GB_log(&gb, "The save path for this ROM is not writeable, progress will not be saved.\n");
        }
    }
    
    char cheat_path[path_length + 5];
    replace_extension(filename, path_length, cheat_path, ".cht");
    GB_load_cheats(&gb, cheat_path);
    
    end_capturing_logs(true, error, SDL_MESSAGEBOX_WARNING, "Warning");
    
    static char start_text[64];
    static char title[17];
    GB_get_rom_title(&gb, title);
    sprintf(start_text, "SameBoy v" GB_VERSION "\n%s\n%08X", title, GB_get_rom_crc32(&gb));
    show_osd_text(start_text);

    /* Configure symbols */
    GB_debugger_load_symbol_file(&gb, resource_path("registers.sym"));
    
    char symbols_path[path_length + 5];
    replace_extension(filename, path_length, symbols_path, ".sym");
    GB_debugger_load_symbol_file(&gb, symbols_path);
        
    screen_size_changed();

	has_active_trace_packet = false;
	vblank_just_occured = false;
	issue_trace_packet();

    /* Run emulation */
    while (true) {
        if (paused || rewind_paused) {
            SDL_WaitEvent(NULL);
            handle_events(&gb);
        }
        else {
            if (do_rewind) {
                GB_rewind_pop(&gb);
                if (turbo_down) {
                    GB_rewind_pop(&gb);
                }
                if (!GB_rewind_pop(&gb)) {
                    rewind_paused = true;
                }
                do_rewind = false;
            }
            GB_run(&gb);
        }
        
		if (vblank_just_occured)
		{
			issue_trace_packet();
			GB_set_key_mask(&gb, key_mask);
			vblank_just_occured = false;
		}

        /* These commands can't run in the handle_event function, because they're not safe in a vblank context. */
        if (handle_pending_command()) {
            pending_command = GB_SDL_NO_COMMAND;
            goto restart;
        }
        pending_command = GB_SDL_NO_COMMAND;
    }
}

static char prefs_path[PATH_MAX + 1] = {0, };

static void save_configuration(void)
{
    FILE *prefs_file = fopen(prefs_path, "wb");
    if (prefs_file) {
        fwrite(&configuration, 1, sizeof(configuration), prefs_file);
        fclose(prefs_file);
    }
}

static void stop_recording(void)
{
    GB_stop_audio_recording(&gb);
}

static bool get_arg_flag(const char *flag, int *argc, char **argv)
{
    for (unsigned i = 1; i < *argc; i++) {
        if (strcmp(argv[i], flag) == 0) {
            (*argc)--;
            argv[i] = argv[*argc];
            return true;
        }
    }
    return false;
}

static const char *get_arg_option(const char *option, int *argc, char **argv)
{
    for (unsigned i = 1; i < *argc - 1; i++) {
        if (strcmp(argv[i], option) == 0) {
            const char *ret = argv[i + 1];
            memmove(argv + i, argv + i + 2, (*argc - i - 2) * sizeof(argv[0]));
            (*argc) -= 2;
            return ret;
        }
    }
    return NULL;
}

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
static void enable_smooth_scrolling(void)
{
    CFPreferencesSetAppValue(CFSTR("AppleMomentumScrollSupported"), kCFBooleanTrue, kCFPreferencesCurrentApplication);
}
#endif

static void handle_model_option(const char *model_string)
{
    static const struct {
        const char *name;
        GB_model_t model;
        const char *description;
    } name_to_model[] = {
        {"dmg-b", GB_MODEL_DMG_B, "Game Boy, DMG-CPU B"},
        {"dmg", GB_MODEL_DMG_B, "Alias of dmg-b"},
        {"sgb-ntsc", GB_MODEL_SGB_NTSC, "Super Game Boy (NTSC)"},
        {"sgb-pal", GB_MODEL_SGB_PAL, "Super Game Boy (PAL"},
        {"sgb2", GB_MODEL_SGB2, "Super Game Boy 2"},
        {"sgb", GB_MODEL_SGB, "Alias of sgb-ntsc"},
        {"mgb", GB_MODEL_MGB, "Game Boy Pocket/Light"},
        {"cgb-0", GB_MODEL_CGB_0, "Game Boy Color, CPU CGB 0"},
        {"cgb-a", GB_MODEL_CGB_A, "Game Boy Color, CPU CGB A"},
        {"cgb-b", GB_MODEL_CGB_B, "Game Boy Color, CPU CGB B"},
        {"cgb-c", GB_MODEL_CGB_C, "Game Boy Color, CPU CGB C"},
        {"cgb-d", GB_MODEL_CGB_D, "Game Boy Color, CPU CGB D"},
        {"cgb-e", GB_MODEL_CGB_E, "Game Boy Color, CPU CGB E"},
        {"cgb", GB_MODEL_CGB_E, "Alias of cgb-e"},
        {"agb-a", GB_MODEL_AGB_A, "Game Boy Advance, CPU AGB A"},
        {"agb", GB_MODEL_AGB_A, "Alias of agb-a"},
        {"gbp-a", GB_MODEL_GBP_A, "Game Boy Player, CPU AGB A"},
        {"gbp", GB_MODEL_GBP_A, "Alias of gbp-a"},
    };
    
    GB_model_t model = -1;
    for (unsigned i = 0; i < sizeof(name_to_model) / sizeof(name_to_model[0]); i++) {
        if (strcmp(model_string, name_to_model[i].name) == 00) {
            model = name_to_model[i].model;
            break;
        }
    }
    if (model == -1) {
        fprintf(stderr, "'%s' is not a valid model. Valid options are:\n", model_string);
        for (unsigned i = 0; i < sizeof(name_to_model) / sizeof(name_to_model[0]); i++) {
            fprintf(stderr, "%s - %s\n", name_to_model[i].name, name_to_model[i].description);
        }
        exit(1);
    }
    
    switch (model) {
        case GB_MODEL_DMG_B:
            configuration.model = MODEL_DMG;
            break;
        case GB_MODEL_SGB_NTSC:
            configuration.model = MODEL_SGB;
            configuration.sgb_revision = SGB_NTSC;
            break;
        case GB_MODEL_SGB_PAL:
            configuration.model = MODEL_SGB;
            configuration.sgb_revision = SGB_PAL;
            break;
        case GB_MODEL_SGB2:
            configuration.model = MODEL_SGB;
            configuration.sgb_revision = SGB_2;
            break;
        case GB_MODEL_MGB:
            configuration.model = MODEL_DMG;
            break;
        case GB_MODEL_CGB_0:
        case GB_MODEL_CGB_A:
        case GB_MODEL_CGB_B:
        case GB_MODEL_CGB_C:
        case GB_MODEL_CGB_D:
        case GB_MODEL_CGB_E:
            configuration.model = MODEL_CGB;
            configuration.cgb_revision = model - GB_MODEL_CGB_0;
            break;
        case GB_MODEL_AGB_A:
        case GB_MODEL_GBP_A:
            configuration.model = MODEL_AGB;
            configuration.agb_revision = model;
            break;
            
        default:
            break;
    }
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetProcessDPIAware();
#endif
#ifdef __APPLE__
    enable_smooth_scrolling();
#endif

    const char *model_string = get_arg_option("--model", &argc, argv);
    bool fullscreen = get_arg_flag("--fullscreen", &argc, argv) || get_arg_flag("-f", &argc, argv);
    bool nogl = get_arg_flag("--nogl", &argc, argv);
    stop_on_start = get_arg_flag("--stop-debugger", &argc, argv) || get_arg_flag("-s", &argc, argv);
    

    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        fprintf(stderr, "SameBoy v" GB_VERSION "\n");
        fprintf(stderr, "Usage: %s [--fullscreen|-f] [--nogl] [--stop-debugger|-s] [--model <model>] <rom>\n", argv[0]);
        exit(1);
    }
    
    if (argc == 2) {
        filename = argv[1];
    }

    signal(SIGINT, debugger_interrupt);
#ifndef _WIN32
    signal(SIGUSR1, debugger_reset);
#endif

    SDL_Init(SDL_INIT_EVERYTHING & ~SDL_INIT_AUDIO);
    // This is, essentially, best-effort.
    // This function will not be called if the process is terminated in any way, anyhow.
    atexit(SDL_Quit);

    if ((console_supported = CON_start(completer))) {
        CON_set_repeat_empty(true);
        CON_printf("SameBoy v" GB_VERSION "\n");
    }
    else {
        fprintf(stderr, "SameBoy v" GB_VERSION "\n");
    }
    
    strcpy(prefs_path, resource_path("prefs.bin"));
    if (access(prefs_path, R_OK | W_OK) != 0) {
        char *prefs_dir = SDL_GetPrefPath("", "SameBoy");
        snprintf(prefs_path, sizeof(prefs_path) - 1, "%sprefs.bin", prefs_dir);
        SDL_free(prefs_dir);
    }
    
    FILE *prefs_file = fopen(prefs_path, "rb");
    if (prefs_file) {
        fread(&configuration, 1, sizeof(configuration), prefs_file);
        fclose(prefs_file);
        
        /* Sanitize for stability */
        configuration.color_correction_mode %= GB_COLOR_CORRECTION_MODERN_ACCURATE + 1;
        configuration.scaling_mode %= GB_SDL_SCALING_MAX;
        configuration.default_scale %= GB_SDL_DEFAULT_SCALE_MAX + 1;
        configuration.blending_mode %= GB_FRAME_BLENDING_MODE_ACCURATE + 1;
        configuration.highpass_mode %= GB_HIGHPASS_MAX;
        configuration.model %= MODEL_MAX;
        configuration.sgb_revision %= SGB_MAX;
        configuration.dmg_palette %= 5;
        if (configuration.dmg_palette) {
            configuration.gui_pallete_enabled = true;
        }
        configuration.border_mode %= GB_BORDER_ALWAYS + 1;
        configuration.rumble_mode %= GB_RUMBLE_ALL_GAMES + 1;
        configuration.color_temperature %= 21;
        configuration.bootrom_path[sizeof(configuration.bootrom_path) - 1] = 0;
        configuration.cgb_revision %= GB_MODEL_CGB_E - GB_MODEL_CGB_0 + 1;
        configuration.audio_driver[15] = 0;
        configuration.dmg_palette_name[24] = 0;
        // Fix broken defaults, should keys 12-31 should be unmapped by default
        if (configuration.joypad_configuration[31] == 0) {
            memset(configuration.joypad_configuration + 12 , -1, 32 - 12);
        }
        if ((configuration.agb_revision & ~GB_MODEL_GBP_BIT) != GB_MODEL_AGB_A) {
            configuration.agb_revision = GB_MODEL_AGB_A;
        }
    }
    
    if (configuration.model >= MODEL_MAX) {
        configuration.model = MODEL_CGB;
    }

    if (configuration.default_scale == 0) {
        configuration.default_scale = 2;
    }
    
    if (model_string) {
        handle_model_option(model_string);
    }
    
    atexit(save_configuration);
    atexit(stop_recording);
    
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
                configuration.allow_background_controllers? "1" : "0");

    window = SDL_CreateWindow("SameBoy v" GB_VERSION, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              160 * configuration.default_scale, 144 * configuration.default_scale, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (window == NULL) {
        fputs(SDL_GetError(), stderr);
        exit(1);
    }
    SDL_SetWindowMinimumSize(window, 160, 144);
    
    if (fullscreen) {
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
    
    gl_context = nogl? NULL : SDL_GL_CreateContext(window);
    
    GLint major = 0, minor = 0;
    if (gl_context) {
        glGetIntegerv(GL_MAJOR_VERSION, &major);
        glGetIntegerv(GL_MINOR_VERSION, &minor);
        update_swap_interval();
    }
    
    if (gl_context && major * 0x100 + minor < 0x302) {
        SDL_GL_DeleteContext(gl_context);
        gl_context = NULL;
    }
    
    if (gl_context == NULL) {
        renderer = SDL_CreateRenderer(window, -1, 0);
        texture = SDL_CreateTexture(renderer, SDL_GetWindowPixelFormat(window), SDL_TEXTUREACCESS_STREAMING, 160, 144);
        pixel_format = SDL_AllocFormat(SDL_GetWindowPixelFormat(window));
    }
    else {
        pixel_format = SDL_AllocFormat(SDL_PIXELFORMAT_ABGR8888);
    }
    
    GB_audio_init();

    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    
    if (!init_shader_with_name(&shader, configuration.filter)) {
        init_shader_with_name(&shader, "NearestNeighbor");
    }
    update_viewport();
    
    if (filename == NULL) {
        stop_on_start = false;
        run_gui(false);
    }
    else {
        connect_joypad();
    }
    GB_audio_set_paused(false);
    run(); // Never returns
    return 0;
}

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include <curl/curl.h>
#include <openssl/sha.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include "nanovg.h"
#define NANOVG_GLES3_IMPLEMENTATION
#include "json11.hpp"
#include "nanovg_gl.h"
#include "nanovg_gl_utils.h"

#include "selfdrive/common/framebuffer.h"
#include "selfdrive/common/touch.h"
#include "selfdrive/common/util.h"

#define USER_AGENT "NEOSUpdater-0.2"

#define MANIFEST_URL_NEOS_STAGING "https://github.com/commaai/eon-neos/raw/master/update.staging.json"
#define MANIFEST_URL_NEOS_LOCAL "http://192.168.5.1:8000/neosupdate/update.local.json"
#define MANIFEST_URL_NEOS "https://github.com/commaai/eon-neos/raw/master/update.json"
const char *manifest_url = MANIFEST_URL_NEOS;

#define RECOVERY_DEV "/dev/block/bootdevice/by-name/recovery"
#define RECOVERY_COMMAND "/cache/recovery/command"

#define UPDATE_DIR "/data/neoupdate"

extern const uint8_t bin_opensans_regular[] asm("_binary_opensans_regular_ttf_start");
extern const uint8_t bin_opensans_regular_end[] asm("_binary_opensans_regular_ttf_end");
extern const uint8_t bin_opensans_semibold[] asm("_binary_opensans_semibold_ttf_start");
extern const uint8_t bin_opensans_semibold_end[] asm("_binary_opensans_semibold_ttf_end");
extern const uint8_t bin_opensans_bold[] asm("_binary_opensans_bold_ttf_start");
extern const uint8_t bin_opensans_bold_end[] asm("_binary_opensans_bold_ttf_end");

namespace {

std::string sha256_file(std::string fn, size_t limit=0) {
  SHA256_CTX ctx;
  SHA256_Init(&ctx);

  FILE *file = fopen(fn.c_str(), "rb");
  if (!file) return "";

  const size_t buf_size = 8192;
  std::unique_ptr<char[]> buffer( new char[ buf_size ] );

  bool read_limit = (limit != 0);
  while (true) {
    size_t read_size = buf_size;
    if (read_limit) read_size = std::min(read_size, limit);
    size_t bytes_read = fread(buffer.get(), 1, read_size, file);
    if (!bytes_read) break;

    SHA256_Update(&ctx, buffer.get(), bytes_read);

    if (read_limit) {
      limit -= bytes_read;
      if (limit == 0) break;
    }
  }

  uint8_t hash[SHA256_DIGEST_LENGTH];
  SHA256_Final(hash, &ctx);

  fclose(file);

  return util::tohex(hash, sizeof(hash));
}

size_t download_string_write(void *ptr, size_t size, size_t nmeb, void *up) {
  size_t sz = size * nmeb;
  ((std::string*)up)->append((char*)ptr, sz);
  return sz;
}

std::string download_string(CURL *curl, std::string url) {
  std::string os;

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 0);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
  curl_easy_setopt(curl, CURLOPT_RESUME_FROM, 0);

  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1);

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_string_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &os);
  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    return "";
  }

  return os;
}

size_t download_file_write(void *ptr, size_t size, size_t nmeb, void *up) {
  return fwrite(ptr, size, nmeb, (FILE*)up);
}

int battery_capacity() {
  std::string bat_cap_s = util::read_file("/sys/class/power_supply/battery/capacity");
  return atoi(bat_cap_s.c_str());
}

int battery_current() {
  std::string current_now_s = util::read_file("/sys/class/power_supply/battery/current_now");
  return atoi(current_now_s.c_str());
}

bool has_no_battery() {
  if (FILE *file = fopen("/data/params/d/dp_no_batt", "r")) {
    fclose(file);
    std::string has_no_batt = util::read_file("/data/params/d/dp_no_batt");
    return atoi(has_no_batt.c_str()) == 1;
  } else {
    return false;
  }
}

bool check_battery() {
  int bat_cap = battery_capacity();
  int current_now = battery_current();
  return (has_no_battery())? true : bat_cap > 35 || (current_now < 0 && bat_cap > 10);
}

bool check_space() {
  struct statvfs stat;
  if (statvfs("/data/", &stat) != 0) {
    return false;
  }
  size_t space = stat.f_bsize * stat.f_bavail;
  return space > 2000000000ULL; // 2GB
}

static void start_settings_activity(const char* name) {
  char launch_cmd[1024];
  snprintf(launch_cmd, sizeof(launch_cmd),
           "am start -W --ez :settings:show_fragment_as_subsetting true -n 'com.android.settings/.%s'", name);
  system(launch_cmd);
}

bool is_settings_active() {
  FILE *fp;
  char sys_output[4096];

  fp = popen("/bin/dumpsys window windows", "r");
  if (fp == NULL) {
    return false;
  }

  bool active = false;
  while (fgets(sys_output, sizeof(sys_output), fp) != NULL) {
    if (strstr(sys_output, "mCurrentFocus=null")  != NULL) {
      break;
    }

    if (strstr(sys_output, "mCurrentFocus=Window") != NULL) {
      active = true;
      break;
    }
  }

  pclose(fp);

  return active;
}

struct Updater {
  bool do_exit = false;

  TouchState touch;

  int fb_w, fb_h;

  std::unique_ptr<FrameBuffer> fb;
  NVGcontext *vg = NULL;
  int font_regular;
  int font_semibold;
  int font_bold;

  std::thread update_thread_handle;

  std::mutex lock;

  enum UpdateState {
    CONFIRMATION,
    LOW_BATTERY,
    RUNNING,
    ERROR,
  };
  UpdateState state;

  std::string progress_text;
  float progress_frac;

  std::string error_text;

  std::string low_battery_text;
  std::string low_battery_title;
  std::string low_battery_context;
  std::string battery_cap_text;
  int min_battery_cap = 35;

  // button
  int b_x, b_w, b_y, b_h;
  int balt_x;

  // download stage writes these for the installation stage
  int recovery_len;
  std::string recovery_hash;
  std::string recovery_fn;
  std::string ota_fn;

  CURL *curl = NULL;

  void ui_init() {
    touch_init(&touch);

    fb = std::make_unique<FrameBuffer>("updater", 0x00001000, false, &fb_w, &fb_h);

    fb->set_power(HWC_POWER_MODE_NORMAL);

    vg = nvgCreateGLES3(NVG_ANTIALIAS | NVG_STENCIL_STROKES | NVG_DEBUG);
    assert(vg);

    font_regular = nvgCreateFontMem(vg, "opensans_regular", (unsigned char*)bin_opensans_regular, (bin_opensans_regular_end - bin_opensans_regular), 0);
    assert(font_regular >= 0);

    font_semibold = nvgCreateFontMem(vg, "opensans_semibold", (unsigned char*)bin_opensans_semibold, (bin_opensans_semibold_end - bin_opensans_semibold), 0);
    assert(font_semibold >= 0);

    font_bold = nvgCreateFontMem(vg, "opensans_bold", (unsigned char*)bin_opensans_bold, (bin_opensans_bold_end - bin_opensans_bold), 0);
    assert(font_bold >= 0);

    b_w = 640;
    balt_x = 200;
    b_x = fb_w-b_w-200;
    b_y = 720;
    b_h = 220;

    if (download_stage(true)) {
      state = RUNNING;
      update_thread_handle = std::thread(&Updater::run_stages, this);
    } else {
      state = CONFIRMATION;
    }
  }

  int download_file_xferinfo(curl_off_t dltotal, curl_off_t dlno,
                             curl_off_t ultotal, curl_off_t ulnow) {
    {
      std::lock_guard<std::mutex> guard(lock);
      if (dltotal != 0) {
        progress_frac = (float) dlno / dltotal;
      }
    }
    // printf("info: %ld %ld %f\n", dltotal, dlno, progress_frac);
    return 0;
  }

  bool download_file(std::string url, std::string out_fn) {
    FILE *of = fopen(out_fn.c_str(), "ab");
    assert(of);

    CURLcode res;
    long last_resume_from = 0;

    fseek(of, 0, SEEK_END);

    int tries = 4;

    bool ret = false;

    while (true) {
      long resume_from = ftell(of);

      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 0);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, USER_AGENT);
      curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1);
      curl_easy_setopt(curl, CURLOPT_RESUME_FROM, resume_from);

      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, download_file_write);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, of);

      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0);

      curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
      curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &Updater::download_file_xferinfo);

      CURLcode res = curl_easy_perform(curl);

      long response_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);

      // double content_length = 0.0;
      // curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &content_length);

      printf("download %s res %d, code %ld, resume from %ld\n", url.c_str(), res, response_code, resume_from);
      if (res == CURLE_OK) {
        ret = true;
        break;
      } else if (res == CURLE_HTTP_RETURNED_ERROR && response_code == 416) {
        // failed because the file is already complete?
        ret = true;
        break;
      } else if (resume_from == last_resume_from) {
        // failed and dind't make make forward progress. only retry a couple times
        tries--;
        if (tries <= 0) {
          break;
        }
      }
      last_resume_from = resume_from;
    }
    // printf("res %d\n", res);

    // printf("- %ld %f\n", response_code, content_length);

    fclose(of);

    return ret;
  }

  void set_progress(std::string text) {
    std::lock_guard<std::mutex> guard(lock);
    progress_text = text;
  }

  void set_error(std::string text) {
    std::lock_guard<std::mutex> guard(lock);
    error_text = text;
    state = ERROR;
  }

  void set_battery_low() {
    std::lock_guard<std::mutex> guard(lock);
    state = LOW_BATTERY;
  }

  void set_running() {
    std::lock_guard<std::mutex> guard(lock);
    state = RUNNING;
  }

  std::string download(std::string url, std::string hash, std::string name, bool dry_run) {
    std::string out_fn = UPDATE_DIR "/" + util::base_name(url);

    std::string fn_hash = sha256_file(out_fn);
    if (dry_run) {
      return (hash.compare(fn_hash) != 0) ? "" : out_fn;
    }

    // start or resume downloading if hash doesn't match
    if (hash.compare(fn_hash) != 0) {
      set_progress("Downloading " + name + "...");
      bool r = download_file(url, out_fn);
      if (!r) {
        set_error("failed to download " + name);
        unlink(out_fn.c_str());
        return "";
      }
      fn_hash = sha256_file(out_fn);
    }

    set_progress("Verifying " + name + "...");
    printf("got %s hash: %s\n", name.c_str(), hash.c_str());
    if (fn_hash != hash) {
      set_error(name + " was corrupt");
      unlink(out_fn.c_str());
      return "";
    }
    return out_fn;
  }

  bool download_stage(bool dry_run = false) {
    curl = curl_easy_init();
    assert(curl);

    // ** quick checks before download **

    if (!check_space()) {
      if (!dry_run) set_error("2GB of free space required to update");
      return false;
    }

    mkdir(UPDATE_DIR, 0777);

    set_progress("Finding latest version...");
    std::string manifest_s = download_string(curl, manifest_url);
    printf("manifest: %s\n", manifest_s.c_str());

    std::string err;
    auto manifest = json11::Json::parse(manifest_s, err);
    if (manifest.is_null() || !err.empty()) {
      set_error("failed to load update manifest");
      return false;
    }

    std::string ota_url = manifest["ota_url"].string_value();
    std::string ota_hash = manifest["ota_hash"].string_value();

    std::string recovery_url = manifest["recovery_url"].string_value();
    recovery_hash = manifest["recovery_hash"].string_value();
    recovery_len = manifest["recovery_len"].int_value();

    // std::string installer_url = manifest["installer_url"].string_value();
    // std::string installer_hash = manifest["installer_hash"].string_value();

    if (ota_url.empty() || ota_hash.empty()) {
      set_error("invalid update manifest");
      return false;
    }

    // std::string installer_fn = download(installer_url, installer_hash, "installer");
    // if (installer_fn.empty()) {
    //   //error'd
    //   return;
    // }

    // ** handle recovery download **
    if (recovery_url.empty() || recovery_hash.empty() || recovery_len == 0) {
      set_progress("Skipping recovery flash...");
    } else {
      // only download the recovery if it differs from what's flashed
      set_progress("Checking recovery...");
      std::string existing_recovery_hash = sha256_file(RECOVERY_DEV, recovery_len);
      printf("existing recovery hash: %s\n", existing_recovery_hash.c_str());

      if (existing_recovery_hash != recovery_hash) {
        recovery_fn = download(recovery_url, recovery_hash, "recovery", dry_run);
        if (recovery_fn.empty()) {
          // error'd
          return false;
        }
      }
    }

    // ** handle ota download **
    ota_fn = download(ota_url, ota_hash, "update", dry_run);
    if (ota_fn.empty()) {
      //error'd
      return false;
    }

    // download sucessful
    return true;
  }

  // thread that handles downloading and installing the update
  void run_stages() {
    printf("run_stages start\n");


    // ** download update **

    if (!check_battery()) {
      set_battery_low();
      int battery_cap = battery_capacity();
      while(battery_cap < min_battery_cap) {
        battery_cap = battery_capacity();
        battery_cap_text = std::to_string(battery_cap);
        util::sleep_for(1000);
      }
      set_running();
    }

    bool sucess = download_stage();
    if (!sucess) {
      return;
    }

    // ** install update **

    if (!check_battery()) {
      set_battery_low();
      int battery_cap = battery_capacity();
      while(battery_cap < min_battery_cap) {
        battery_cap = battery_capacity();
        battery_cap_text = std::to_string(battery_cap);
        util::sleep_for(1000);
      }
      set_running();
    }

    if (!recovery_fn.empty()) {
      // flash recovery
      set_progress("Flashing recovery...");

      FILE *flash_file = fopen(recovery_fn.c_str(), "rb");
      if (!flash_file) {
        set_error("failed to flash recovery");
        return;
      }

      FILE *recovery_dev = fopen(RECOVERY_DEV, "w+b");
      if (!recovery_dev) {
        fclose(flash_file);
        set_error("failed to flash recovery");
        return;
      }

      const size_t buf_size = 4096;
      std::unique_ptr<char[]> buffer( new char[ buf_size ] );

      while (true) {
        size_t bytes_read = fread(buffer.get(), 1, buf_size, flash_file);
        if (!bytes_read) break;

        size_t bytes_written = fwrite(buffer.get(), 1, bytes_read, recovery_dev);
        if (bytes_read != bytes_written) {
          fclose(recovery_dev);
          fclose(flash_file);
          set_error("failed to flash recovery: write failed");
          return;
        }
      }

      fclose(recovery_dev);
      fclose(flash_file);

      set_progress("Verifying flash...");
      std::string new_recovery_hash = sha256_file(RECOVERY_DEV, recovery_len);
      printf("new recovery hash: %s\n", new_recovery_hash.c_str());

      if (new_recovery_hash != recovery_hash) {
        set_error("recovery flash corrupted");
        return;
      }

    }

    // write arguments to recovery
    FILE *cmd_file = fopen(RECOVERY_COMMAND, "wb");
    if (!cmd_file) {
      set_error("failed to reboot into recovery");
      return;
    }
    fprintf(cmd_file, "--update_package=%s\n", ota_fn.c_str());
    fclose(cmd_file);

    set_progress("Rebooting");

    // remove the continue.sh so we come back into the setup.
    // maybe we should go directly into the installer, but what if we don't come back with internet? :/
    //unlink("/data/data/com.termux/files/continue.sh");

    // TODO: this should be generic between android versions
    // IPowerManager.reboot(confirm=false, reason="recovery", wait=true)
    system("service call power 16 i32 0 s16 recovery i32 1");
    while (true) pause();

    // execl("/system/bin/reboot", "recovery");
    // set_error("failed to reboot into recovery");
  }

  void draw_ack_screen(const char *title, const char *message, const char *button, const char *altbutton) {
    nvgFillColor(vg, nvgRGBA(255,255,255,255));
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);

    nvgFontFace(vg, "opensans_bold");
    nvgFontSize(vg, 120.0f);
    nvgTextBox(vg, 110, 220, fb_w-240, title, NULL);

    nvgFontFace(vg, "opensans_regular");
    nvgFontSize(vg, 86.0f);
    nvgTextBox(vg, 130, 380, fb_w-260, message, NULL);

    // draw button
    if (button) {
      nvgBeginPath(vg);
      nvgFillColor(vg, nvgRGBA(8, 8, 8, 255));
      nvgRoundedRect(vg, b_x, b_y, b_w, b_h, 20);
      nvgFill(vg);

      nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
      nvgFontFace(vg, "opensans_semibold");
      nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
      nvgText(vg, b_x+b_w/2, b_y+b_h/2, button, NULL);

      nvgBeginPath(vg);
      nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 50));
      nvgStrokeWidth(vg, 5);
      nvgRoundedRect(vg, b_x, b_y, b_w, b_h, 20);
      nvgStroke(vg);
    }

    // draw button
    if (altbutton) {
      nvgBeginPath(vg);
      nvgFillColor(vg, nvgRGBA(8, 8, 8, 255));
      nvgRoundedRect(vg, balt_x, b_y, b_w, b_h, 20);
      nvgFill(vg);

      nvgFillColor(vg, nvgRGBA(255, 255, 255, 255));
      nvgFontFace(vg, "opensans_semibold");
      nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
      nvgText(vg, balt_x+b_w/2, b_y+b_h/2, altbutton, NULL);

      nvgBeginPath(vg);
      nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 50));
      nvgStrokeWidth(vg, 5);
      nvgRoundedRect(vg, balt_x, b_y, b_w, b_h, 20);
      nvgStroke(vg);
    }
  }

  void draw_battery_screen() {
    low_battery_title = "Low Battery";
    low_battery_text = "Please connect EON to your charger. Update will continue once EON battery reaches 35%.";
    low_battery_context = "Current battery charge: " + battery_cap_text + "%";

    nvgFillColor(vg, nvgRGBA(255,255,255,255));
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);

    nvgFontFace(vg, "opensans_bold");
    nvgFontSize(vg, 120.0f);
    nvgTextBox(vg, 110, 220, fb_w-240, low_battery_title.c_str(), NULL);

    nvgFontFace(vg, "opensans_regular");
    nvgFontSize(vg, 86.0f);
    nvgTextBox(vg, 130, 380, fb_w-260, low_battery_text.c_str(), NULL);

    nvgFontFace(vg, "opensans_bold");
    nvgFontSize(vg, 86.0f);
    nvgTextBox(vg, 130, 700, fb_w-260, low_battery_context.c_str(), NULL);
  }

  void draw_progress_screen() {
    // draw progress message
    nvgFontSize(vg, 64.0f);
    nvgFillColor(vg, nvgRGBA(255,255,255,255));
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_BASELINE);
    nvgFontFace(vg, "opensans_bold");
    nvgFontSize(vg, 86.0f);
    nvgTextBox(vg, 0, 380, fb_w, progress_text.c_str(), NULL);

    // draw progress bar
    {
      int progress_width = 1000;
      int progress_x = fb_w/2-progress_width/2;
      int progress_y = 520;
      int progress_height = 50;

      int powerprompt_y = 312;
      nvgFontFace(vg, "opensans_regular");
      nvgFontSize(vg, 64.0f);
      nvgText(vg, fb_w/2, 740, "Ensure your device remains connected to a power source.", NULL);

      NVGpaint paint = nvgBoxGradient(
          vg, progress_x + 1, progress_y + 1,
          progress_width - 2, progress_height, 3, 4, nvgRGB(27, 27, 27), nvgRGB(27, 27, 27));
      nvgBeginPath(vg);
      nvgRoundedRect(vg, progress_x, progress_y, progress_width, progress_height, 12);
      nvgFillPaint(vg, paint);
      nvgFill(vg);

      float value = std::min(std::max(0.0f, progress_frac), 1.0f);
      int bar_pos = ((progress_width - 2) * value);

      paint = nvgBoxGradient(
          vg, progress_x, progress_y,
          bar_pos+1.5f, progress_height-1, 3, 4,
          nvgRGB(245, 245, 245), nvgRGB(105, 105, 105));

      nvgBeginPath(vg);
      nvgRoundedRect(
          vg, progress_x+1, progress_y+1,
          bar_pos, progress_height-2, 12);
      nvgFillPaint(vg, paint);
      nvgFill(vg);
    }
  }

  void ui_draw() {
    std::lock_guard<std::mutex> guard(lock);

    nvgBeginFrame(vg, fb_w, fb_h, 1.0f);

    switch (state) {
    case CONFIRMATION:
      draw_ack_screen("An update to NEOS is required.",
                      "Your device will now be reset and upgraded. You may want to connect to wifi as download is around 1 GB. Existing data on device should not be lost.",
                      "Continue",
                      "Connect to WiFi");
      break;
    case LOW_BATTERY:
      draw_battery_screen();
      break;
    case RUNNING:
      draw_progress_screen();
      break;
    case ERROR:
      draw_ack_screen("There was an error", (error_text).c_str(), NULL, "Reboot");
      break;
    }

    nvgEndFrame(vg);
  }

  void ui_update() {
    std::lock_guard<std::mutex> guard(lock);

    if (state == ERROR || state == CONFIRMATION) {
      int touch_x = -1, touch_y = -1;
      int res = touch_poll(&touch, &touch_x, &touch_y, 0);
      if (res == 1 && !is_settings_active()) {
        if (touch_x >= b_x && touch_x < b_x+b_w && touch_y >= b_y && touch_y < b_y+b_h) {
          if (state == CONFIRMATION) {
            state = RUNNING;
            update_thread_handle = std::thread(&Updater::run_stages, this);
          }
        }
        if (touch_x >= balt_x && touch_x < balt_x+b_w && touch_y >= b_y && touch_y < b_y+b_h) {
          if (state == CONFIRMATION) {
            start_settings_activity("Settings$WifiSettingsActivity");
          } else if (state == ERROR) {
            do_exit = 1;
          }
        }
      }
    }
  }

  void go() {
    ui_init();

    while (!do_exit) {
      ui_update();

      glClearColor(0.08, 0.08, 0.08, 1.0);
      glClear(GL_STENCIL_BUFFER_BIT | GL_COLOR_BUFFER_BIT);

      // background
      nvgBeginPath(vg);
      NVGpaint bg = nvgLinearGradient(vg, fb_w, 0, fb_w, fb_h,
        nvgRGBA(0, 0, 0, 0), nvgRGBA(0, 0, 0, 255));
      nvgFillPaint(vg, bg);
      nvgRect(vg, 0, 0, fb_w, fb_h);
      nvgFill(vg);

      glEnable(GL_BLEND);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

      ui_draw();

      glDisable(GL_BLEND);

      fb->swap();

      assert(glGetError() == GL_NO_ERROR);

      // no simple way to do 30fps vsync with surfaceflinger...
      util::sleep_for(30);
    }

    if (update_thread_handle.joinable()) {
      update_thread_handle.join();
    }

    // reboot
    system("service call power 16 i32 0 i32 0 i32 1");
  }

};

}

int main(int argc, char *argv[]) {
  bool background_cache = false;
  if (argc > 1) {
    if (strcmp(argv[1], "local") == 0) {
      manifest_url = MANIFEST_URL_NEOS_LOCAL;
    } else if (strcmp(argv[1], "staging") == 0) {
      manifest_url = MANIFEST_URL_NEOS_STAGING;
    } else if (strcmp(argv[1], "bgcache") == 0) {
      manifest_url = argv[2];
      background_cache = true;
    } else {
      manifest_url = argv[1];
    }
  }

  printf("updating from %s\n", manifest_url);
  Updater updater;

  int err = 0;
  if (background_cache) {
    err = !updater.download_stage();
  } else {
    updater.go();
  }
  return err;
}

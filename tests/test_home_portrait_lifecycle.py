"""Exercise production portrait allocation, rendering fallback, refresh and release."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'main/apps/home/app_home.cc').read_text()
read = source[source.index('esp_err_t read_portrait'):source.index('// 显示用 A8')]
apply = source[source.index('void expand_portrait'):source.index('// 电量画成一个')]
cleanup = source[source.index('    if (s_portrait_pixels != nullptr)', source.index('void home_stop()')):
                 source.index('    ESP_LOGI(kTag, "Home app stopped', source.index('void home_stop()'))]
prelude = r'''
#include <cassert>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <set>
#define HOME_AVATAR_BYTES 4976
#define HOME_PORTRAIT_PIXELS 14400
#define HOME_PORTRAIT_I4_BYTES 7264
#define HOME_PORTRAIT_W 96
#define HOME_PORTRAIT_H 138
#define LV_COLOR_FORMAT_A8 1
#define LV_COLOR_FORMAT_I4 2
#define LV_OBJ_FLAG_HIDDEN 1
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NOT_FOUND -2
#define ESP_ERR_NO_MEM -3
#define ESP_LOGW(...) ((void)0)
using esp_err_t=int;
static uint8_t *s_portrait_packed,*s_portrait_pixels;
static struct {struct {int cf,stride;}header;size_t data_size;const void *data;}s_portrait_dsc;
static int image,placeholder;
static void *s_avatar_img=&image,*s_portrait_ph=&placeholder;
static const void *displayed;
static bool hidden=true,fail_read;
static int allocations,fail_at,version=1;
static std::set<void*>live;
static void *allocate(size_t n){if(++allocations==fail_at)return nullptr;void*p=malloc(n);assert(p);live.insert(p);return p;}
static void release(void*p){if(p){assert(p!=displayed);assert(live.erase(p)==1);free(p);}}
static int home_profile_read_avatar(uint8_t*p,size_t n){assert(n==4976);if(fail_read)return ESP_FAIL;memset(p,0,n);p[4]=version;return ESP_OK;}
static void lv_image_set_src(void*,const void*p){displayed=p;}
static void lv_image_cache_drop(const void*){}
static void lv_obj_set_style_image_recolor(void*,int,int){}
static int lv_color_hex(int v){return v;}
static int ui_theme_portrait_color(int v){return v;}
static void lv_obj_clear_flag(void*p,int){if(p==s_avatar_img)hidden=false;}
static void lv_obj_add_flag(void*p,int){if(p==s_avatar_img)hidden=true;}
static inline int home_portrait_index(const uint8_t*data,size_t pixel){return (data[8+pixel/4]>>(6-2*(pixel%4)))&3;}
struct lv_color32_t{uint8_t b,g,r,a;};
#define LV_OPA_COVER 255
static lv_color32_t lv_color_to_32(int,int){return {0,0,0,255};}
static int ui_theme_portrait_subject_color(int){return 0;}
#define malloc allocate
#define free release
'''
checks = r'''
int main(){
 assert(read_portrait(false)==ESP_FAIL && !allocations);apply_avatar(false);assert(hidden);
 fail_at=1;assert(read_portrait(true)==ESP_ERR_NO_MEM);apply_avatar(false);assert(hidden&&!displayed);
 fail_at=0;assert(read_portrait(true)==ESP_OK);fail_at=allocations+1;
 apply_avatar(true);assert(hidden&&!displayed&&live.size()==1);
 fail_at=0;apply_avatar(true);assert(!hidden&&s_portrait_dsc.data==s_portrait_pixels&&live.size()==2);
 int before=allocations;assert(read_portrait(true)==ESP_OK);apply_avatar(true);assert(allocations==before);
 version=0;assert(read_portrait(true)==ESP_OK);apply_avatar(true);assert(!hidden&&s_portrait_dsc.data_size==14400&&live.size()==2);
 fail_read=true;apply_avatar(read_portrait(true)==ESP_OK);assert(hidden&&!displayed);
 cleanup();assert(!s_portrait_pixels&&!s_portrait_packed&&!s_portrait_dsc.data&&live.empty());
 cleanup();fail_read=false;version=1;assert(read_portrait(true)==ESP_OK);apply_avatar(true);
 lv_image_set_src(s_avatar_img,nullptr);cleanup();assert(live.empty());
}
'''
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'home.cpp'
    path.write_text(prelude + read + apply + '\nstatic void cleanup(){\n' + cleanup + '}\n' + checks)
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    str(path), '-o', str(path.with_suffix(''))], check=True)
    subprocess.run([str(path.with_suffix(''))], check=True)
print('Home portrait: absent/OOM/read failure, refresh/reformat, release and re-entry: PASS')

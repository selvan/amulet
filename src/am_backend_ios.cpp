#include "amulet.h"

#ifdef AM_BACKEND_IOS

#import <objc/runtime.h>
#import <QuartzCore/QuartzCore.h>

#import <OpenGLES/EAGL.h>
#import <OpenGLES/EAGLDrawable.h>
#import <QuartzCore/QuartzCore.h>
#import <UIKit/UIKit.h>
#import <AudioToolbox/AudioServices.h>
#import <AudioUnit/AudioUnit.h>
#import <CoreMotion/CoreMotion.h>
#import <GLKit/GLKit.h>
#import <GameKit/GameKit.h>

#include <OpenGLES/ES2/gl.h>
#include <OpenGLES/ES2/glext.h>

#include <mach/mach_time.h>

static am_engine *ios_eng = NULL;
static am_package *package = NULL;
static am_display_orientation ios_orientation = AM_DISPLAY_ORIENTATION_ANY;
static bool ios_window_created = false;
static bool ios_running = false;
static bool ios_audio_initialized = false;
static bool ios_audio_paused = false;
static float *ios_audio_buffer = NULL;
static int ios_audio_offset = 0;
static NSLock *ios_audio_mutex = [[NSLock alloc] init];
static bool ios_done_first_draw = false;

static GLKView *ios_view = nil;
static GLKViewController *ios_view_controller = nil;

static double t0;
static double t_debt;
static double frame_time = 0.0;
static double delta_time;
static double real_delta_time;

//static void init_gamecenter();

#define MIN_UPDATE_TIME (1.0/400.0)

//---------------------------------------------------------------------------

/*
static float ios_scaling() {
    float scale = 1.0f;
    if([[UIScreen mainScreen] respondsToSelector: NSSelectorFromString(@"scale")]) {
        scale = [[UIScreen mainScreen] scale];
    }
    return scale;
}

static bool ios_is_ipad() {
    if (UI_USER_INTERFACE_IDIOM() == UIUserInterfaceIdiomPad) {
        return true;
    } else {
        return false;
    }
}

static bool ios_is_retina_iphone() {
    return !ios_is_ipad() && ios_scaling() >= 2.0f;
}
*/

/*
static const char *ios_doc_path(const char *file, const char *suffix) {
    NSArray *dirs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    const char *dir = [[dirs objectAtIndex:0] UTF8String];
    int len = strlen(dir) + strlen(file) + 2;
    if (suffix != NULL) {
        len += strlen(suffix);
    }
    char *path = (char*)malloc(len);
    if (suffix == NULL) {
        snprintf(path, len, "%s/%s", dir, file);
    } else {
        snprintf(path, len, "%s/%s%s", dir, file, suffix);
    }
    return path;
}
*/

static const char *ios_bundle_path() {
    return [[[NSBundle mainBundle] bundlePath] UTF8String];
}

//------------- prefs store ----------------

static NSUserDefaults *prefs = nil;

static void ensure_prefs_initialized() {
    if (prefs == nil) {
        prefs = [NSUserDefaults standardUserDefaults];
    }
}

static void ios_store_pref_data(const char *key, const char *val) {
    ensure_prefs_initialized();
    NSString *skey = [NSString stringWithUTF8String:key];
    NSString *sval = [NSString stringWithUTF8String:val];
    [prefs setObject:sval forKey:skey];
}

static int ios_store_pref(lua_State *L) {
    am_check_nargs(L, 2);
    const char *key = lua_tostring(L, 1);
    const char *val = lua_tostring(L, 2);
    if (key == NULL) return luaL_error(L, "expecting a string in position 1");
    if (val == NULL) return luaL_error(L, "expecting a string in position 2");
    ios_store_pref_data(key, val);
    return 0;
}

static char* ios_retrieve_pref_data(const char *key) {
    ensure_prefs_initialized();
    NSString *sval = [prefs stringForKey:[NSString stringWithUTF8String:key]];
    if (sval != nil) {
        const char *val0 = [sval UTF8String];
        char *val = (char*)malloc(strlen(val0) + 1);
        strcpy(val, val0);
        return val;
    } else {
        return NULL;
    }
}

static int ios_retrieve_pref(lua_State *L) {
    am_check_nargs(L, 1);
    const char *key = lua_tostring(L, 1);
    if (key == NULL) return luaL_error(L, "expecting a string in position 1");
    char *val = ios_retrieve_pref_data(key);
    if (val == NULL) {
        lua_pushnil(L);
    } else {
        lua_pushstring(L, val);
        free(val);
    }
    return 1;
}

static void ios_sync_store() {
    if (prefs != nil) {
        [prefs synchronize];
    }
}

/*
static void ios_launch_url(const char *url) {
    NSString *ns_url = [NSString stringWithUTF8String:url];
    [[UIApplication sharedApplication] openURL:[NSURL URLWithString:ns_url]];
}
*/

// ---------------------------------------------------------------------------


static void set_audio_category() {
    UInt32 category = kAudioSessionCategory_AmbientSound;
    AudioSessionSetProperty(kAudioSessionProperty_AudioCategory, sizeof(category), &category);
}

static void ios_audio_suspend() {
    ios_audio_paused = true;
}

static void ios_audio_resume() {
    ios_audio_paused = false;
}

static void audio_interrupt(void *ud, UInt32 state) {
    if (state == kAudioSessionBeginInterruption) {
        AudioSessionSetActive(NO);
        ios_audio_suspend();
    } else if (state == kAudioSessionEndInterruption) {
        set_audio_category();
        AudioSessionSetActive(YES);
        ios_audio_resume();
    }
}

static bool open_package() {
    char *package_filename = am_format("%s/%s", am_opt_data_dir, "data.pak");
    if (am_file_exists(package_filename)) {
        char *errmsg;
        package = am_open_package(package_filename, &errmsg);
        if (package == NULL) {
            am_log0("%s", errmsg);
            free(errmsg);
            free(package_filename);
            return false;
        }
    }
    free(package_filename);
    return true;
}

static OSStatus
ios_audio_callback(void *inRefCon,
               AudioUnitRenderActionFlags * ioActionFlags,
               const AudioTimeStamp * inTimeStamp,
               UInt32 inBusNumber, UInt32 inNumberFrames,
               AudioBufferList * ioData)
{
    if (!ios_audio_initialized || ios_audio_paused) {
        for (int i = 0; i < ioData->mNumberBuffers; i++) {
            AudioBuffer *abuf = &ioData->mBuffers[i];
            // silence.
            memset(abuf->mData, 0, abuf->mDataByteSize);
        }
        return noErr;
    }

    int num_channels = am_conf_audio_channels;
    int num_samples = am_conf_audio_buffer_size;
    for (int i = 0; i < ioData->mNumberBuffers; i++) {
        AudioBuffer *abuf = &ioData->mBuffers[i];
        int remaining = abuf->mDataByteSize / (sizeof(float) * num_channels);
        float *ptr = (float*)abuf->mData;
        while (remaining > 0) {
            if (ios_audio_offset >= num_samples) {
                // Generate the data
                memset(ios_audio_buffer, 0, num_samples * num_channels * sizeof(float));
                am_audio_bus bus(num_channels, num_samples, ios_audio_buffer);
                [ios_audio_mutex lock];
                am_fill_audio_bus(&bus);
                [ios_audio_mutex unlock];
                ios_audio_offset = 0;
            }

            int len = num_samples - ios_audio_offset;
            if (len > remaining) {
                len = remaining;
            }
            // CoreAudio expects channels to be interleaved
            am_interleave_audio(ptr, ios_audio_buffer,
                num_channels, num_samples, ios_audio_offset, len);
            //memcpy(ptr, (char *)ios_audio_buffer_interleaved + ios_audio_offset, len);
            /*
            for (int k = 0; k < len/4; k++) {
                fprintf(stderr, "ptr[%d] = %f", k, ((float*)ptr)[k]);
            }
            */
            ptr = ptr + len * num_channels;
            remaining -= len;
            ios_audio_offset += len;
        }
    }

    return noErr;
}

static void ios_init_audio() {
    AURenderCallbackStruct callback;
    AudioStreamBasicDescription strdesc;
    AudioComponentDescription desc;
    AudioComponent comp = NULL;
    AudioComponentInstance instance;
    const AudioUnitElement output_bus = 0;
    const AudioUnitElement bus = output_bus;
    const AudioUnitScope scope = kAudioUnitScope_Input;

    AudioSessionInitialize(NULL, NULL, audio_interrupt, nil);
    set_audio_category();
    AudioSessionSetActive(YES);

    memset(&desc, 0, sizeof(AudioComponentDescription));
    desc.componentType = kAudioUnitType_Output;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
    comp = AudioComponentFindNext(NULL, &desc);
    if (comp == NULL) {
        am_log0("%s", "WARNING: unable to find suitable audio component");
        return;
    }

    if (noErr != AudioComponentInstanceNew(comp, &instance)) {
        am_log0("%s", "WARNING: failed to create audio instance");
        return;
    }

    // set format
    memset(&strdesc, 0, sizeof(AudioStreamBasicDescription));
    strdesc.mFormatID = kAudioFormatLinearPCM;
    strdesc.mFormatFlags = kLinearPCMFormatFlagIsPacked | kLinearPCMFormatFlagIsFloat;
    strdesc.mChannelsPerFrame = am_conf_audio_channels;
    strdesc.mSampleRate = am_conf_audio_sample_rate;
    strdesc.mFramesPerPacket = 1;
    strdesc.mBitsPerChannel = 8 * sizeof(float);
    strdesc.mBytesPerFrame =
        strdesc.mBitsPerChannel * strdesc.mChannelsPerFrame / 8;
    strdesc.mBytesPerPacket =
        strdesc.mBytesPerFrame * strdesc.mFramesPerPacket;

    if (noErr != 
        AudioUnitSetProperty(instance, kAudioUnitProperty_StreamFormat,
            scope, bus, &strdesc, sizeof(AudioStreamBasicDescription)))
    {
        am_log0("%s", "WARNING: unable to set audio format");
        return;
    }

    // set callback
    memset(&callback, 0, sizeof(AURenderCallbackStruct));
    callback.inputProc = ios_audio_callback;
    callback.inputProcRefCon = NULL;
    if (noErr != 
        AudioUnitSetProperty(instance, kAudioUnitProperty_SetRenderCallback,
            scope, bus, &callback, sizeof(AURenderCallbackStruct)))
    {
        am_log0("%s", "WARNING: unable to set audio callback");
        return;
    }

    // create buffer
    assert(ios_audio_buffer == NULL);
    ios_audio_buffer = (float*)malloc(am_conf_audio_channels * am_conf_audio_buffer_size * sizeof(float));
    ios_audio_offset = am_conf_audio_buffer_size; // so it gets filled straight away

    ios_audio_initialized = true;

    // start
    if (noErr != AudioUnitInitialize(instance)) {
        am_log0("%s", "WARNING: unable to initialize audio unit");
        return;
    }

    if (noErr != AudioOutputUnitStart(instance)) {
        am_log0("%s", "WARNING: unable to start audio unit");
        return;
    }
}

static void ios_init_engine() {
    ios_running = false;

    am_opt_data_dir = ios_bundle_path();
    am_opt_main_module = "main";
    if (!open_package()) return;
    if (!am_load_config()) return;
    ios_eng = am_init_engine(false, 0, NULL);
    if (ios_eng == NULL) return;

    frame_time = am_get_current_time();
    lua_pushcclosure(ios_eng->L, am_require, 0);
    lua_pushstring(ios_eng->L, am_opt_main_module);
    if (am_call(ios_eng->L, 1, 0)) {
        ios_running = true;
    }
    t0 = am_get_current_time();
    frame_time = t0;
    t_debt = 0.0;
}

static void ios_teardown() {
    ios_running = false;
    if (ios_eng->L != NULL) {
        am_destroy_engine(ios_eng);
        ios_eng = NULL;
    }
    if (am_gl_is_initialized()) {
        am_destroy_gl();
    }
    if (package != NULL) {
        am_close_package(package);
    }
}

// This is used to decide whether rotations should
// be animated (we don't want to animate the initial
// rotation after app launch), as well as whether to
// disable rotations altogether when the app is using
// the accelerometer for input (we want to however
// allow the initial rotation).
static int frames_since_disable_animations = 0;

static void ios_draw() {
    if (ios_eng == NULL) return;
    am_update_windows(ios_eng->L);
    //am_debug("%s", "done drawing");
}

static void ios_update() {
    if (frames_since_disable_animations == 60) {
        [UIView setAnimationsEnabled:YES];
    }
    frames_since_disable_animations++;
    if (!ios_running) return;

    [ios_audio_mutex lock];
    am_sync_audio_graph(ios_eng->L);
    [ios_audio_mutex unlock];

    frame_time = am_get_current_time();
    
    real_delta_time = frame_time - t0;
    if (am_conf_warn_delta_time > 0.0 && real_delta_time > am_conf_warn_delta_time) {
        am_log0("WARNING: FPS dropped to %0.2f (%fs)", 1.0/real_delta_time, real_delta_time);
    }
    // take min in case app paused, or last frame took very long
    delta_time = am_min(am_conf_min_delta_time, real_delta_time); 
    t_debt += delta_time;

    if (am_conf_fixed_delta_time > 0.0) {
        while (t_debt > 0.0) {
            if (!am_execute_actions(ios_eng->L, am_conf_fixed_delta_time)) {
                ios_running = false;
                return;
            }
            t_debt -= am_conf_fixed_delta_time;
        }
    } else {
        if (t_debt > MIN_UPDATE_TIME) {
            if (!am_execute_actions(ios_eng->L, t_debt)) {
                ios_running = false;
                return;
            }
            t_debt = 0.0;
        }
    }

    t0 = frame_time;
}

static void ios_garbage_collect() {
    lua_gc(ios_eng->L, LUA_GCCOLLECT, 0);
    lua_gc(ios_eng->L, LUA_GCCOLLECT, 0);
}


static void ios_touch_began(NSSet *touches) {
    if (ios_eng->L == NULL) return;
    NSEnumerator *e = [touches objectEnumerator];
    UITouch *touch;
    while ((touch = [e nextObject])) {
        CGPoint pos = [touch locationInView:touch.view];
        //am_debug("pos = %f %f", pos.x, pos.y);
        am_find_window((am_native_window*)ios_view)->touch_begin(ios_eng->L, touch, pos.x, pos.y);
    }
}

static void ios_touch_moved(NSSet *touches) {
    if (ios_eng->L == NULL) return;
    NSEnumerator *e = [touches objectEnumerator];
    UITouch *touch;
    while ((touch = [e nextObject])) {
        CGPoint pos = [touch locationInView:touch.view];
        am_find_window((am_native_window*)ios_view)->touch_move(ios_eng->L, touch, pos.x, pos.y);
    }
}

static void ios_touch_ended(NSSet *touches) {
    if (ios_eng->L == NULL) return;
    NSEnumerator *e = [touches objectEnumerator];
    UITouch *touch;
    while ((touch = [e nextObject])) {
        CGPoint pos = [touch locationInView:touch.view];
        am_find_window((am_native_window*)ios_view)->touch_end(ios_eng->L, touch, pos.x, pos.y);
    }
}

static void ios_touch_cancelled(NSSet *touches) {
    ios_touch_ended(touches);
}

/*
static UIViewController *ios_get_view_controller() {
    return (UIViewController*)am_view_controller;
}
*/

static void ios_save_state() {
    // XXX ltSaveState();
    ios_sync_store();
}

static void ios_resign_active() {
    // XXX ltClientShutdown();
}

static void ios_become_active() {
    // XXX ltClientInit();
    frames_since_disable_animations = 0;
    if (ios_view != nil && ios_eng != NULL) {
        // reset event data in case touch end events missing
        am_find_window((am_native_window*)ios_view)->push(ios_eng->L);
        am_call_amulet(ios_eng->L, "_reset_window_event_data", 1, 0);
    }
}

static CMMotionManager *motionManager = nil;

@interface AMViewController : GLKViewController { }
@end

static UIInterfaceOrientation last_orientation = UIInterfaceOrientationLandscapeLeft;
static bool has_rotated = false;

static BOOL handle_orientation(UIInterfaceOrientation orientation) {
    BOOL res = NO;
    if (has_rotated && motionManager != nil && frames_since_disable_animations > 60) {
        // Prevent screen rotation if using motion input.
        return orientation == last_orientation;
    }
    switch (ios_orientation) {
        case AM_DISPLAY_ORIENTATION_PORTRAIT:
            res = (orientation == UIInterfaceOrientationPortrait
                    || orientation == UIInterfaceOrientationPortraitUpsideDown);
            break;
        case AM_DISPLAY_ORIENTATION_LANDSCAPE:
            res = (orientation == UIInterfaceOrientationLandscapeRight
                    || orientation == UIInterfaceOrientationLandscapeLeft);
            break;
        case AM_DISPLAY_ORIENTATION_ANY:
            res = (orientation == UIInterfaceOrientationLandscapeRight
                    || orientation == UIInterfaceOrientationLandscapeLeft
                    || orientation == UIInterfaceOrientationPortrait
                    || orientation == UIInterfaceOrientationPortraitUpsideDown);
            break;
    }
    if (res == YES) {
        has_rotated = true;
        last_orientation = orientation;
    }
    return res;
}

@implementation AMViewController

- (void)dealloc {
    [super dealloc];
}

- (void)didReceiveMemoryWarning {
    [super didReceiveMemoryWarning];
    ios_garbage_collect();
}

- (void)viewDidLoad
{
    [super viewDidLoad];
}

- (void)viewDidUnload
{
    [super viewDidUnload];
}

- (BOOL)shouldAutorotateToInterfaceOrientation:(UIInterfaceOrientation)orientation
{
    //am_debug("%s", "shouldAutorotateToInterfaceOrientation");
    return handle_orientation(orientation);
}

-(NSUInteger)supportedInterfaceOrientations
{
    //am_debug("%s", "supportedInterfaceOrientations");
    switch (ios_orientation) {
        case AM_DISPLAY_ORIENTATION_PORTRAIT: 
            return UIInterfaceOrientationMaskPortrait | UIInterfaceOrientationMaskPortraitUpsideDown;
        case AM_DISPLAY_ORIENTATION_LANDSCAPE:
            return UIInterfaceOrientationMaskLandscapeRight | UIInterfaceOrientationMaskLandscapeLeft;
        case AM_DISPLAY_ORIENTATION_ANY:
            return UIInterfaceOrientationMaskLandscapeRight | UIInterfaceOrientationMaskLandscapeLeft
            | UIInterfaceOrientationMaskPortrait | UIInterfaceOrientationMaskPortraitUpsideDown;
    }
}

- (UIInterfaceOrientation)preferredInterfaceOrientationForPresentation
{
    //am_debug("%s", "preferredInterfaceOrientationForPresentation");
    switch (ios_orientation) {
        case AM_DISPLAY_ORIENTATION_PORTRAIT:
            return UIInterfaceOrientationPortrait;
        case AM_DISPLAY_ORIENTATION_LANDSCAPE:
            return UIInterfaceOrientationLandscapeLeft;
        case AM_DISPLAY_ORIENTATION_ANY:
            return UIInterfaceOrientationPortrait;
    }
}

-(BOOL)prefersStatusBarHidden
{
    return YES;
}

@end


@interface AMAppDelegate : UIResponder <UIApplicationDelegate, GLKViewDelegate, GLKViewControllerDelegate>

@property (strong, nonatomic) UIWindow *window;

@end

@implementation AMAppDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions
{
    // Prevent rotate animation when application launches.
    [UIView setAnimationsEnabled:NO];

    self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
 
    EAGLContext * context = [[EAGLContext alloc] initWithAPI:kEAGLRenderingAPIOpenGLES2];
    GLKView *view = [[GLKView alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
    view.context = context;
    view.delegate = self;
    ios_view = view;
    [view setMultipleTouchEnabled:YES]; 

    [EAGLContext setCurrentContext:context];

    ios_init_engine();
    ios_init_audio();
 
    GLKViewController * viewController = [[AMViewController alloc] initWithNibName:nil bundle:nil];
    viewController.view = view;
    viewController.delegate = self;
    viewController.preferredFramesPerSecond = 60; // XXX
    self.window.rootViewController = viewController;
    ios_view_controller = viewController;
 
    //self.window.backgroundColor = [UIColor whiteColor];
    [self.window makeKeyAndVisible];
    //init_gamecenter();
    return YES;
}

- (void)glkView:(GLKView *)view drawInRect:(CGRect)rect {
    //am_debug("%s", "draw"); 
    ios_draw();
    ios_done_first_draw = true;
}

- (void)glkViewControllerUpdate:(GLKViewController *)controller {
    //am_debug("%s", "update"); 
    if (ios_done_first_draw) {
        // make sure we do a draw before our first update
        ios_update();
    }
}

- (void)applicationDidEnterBackground:(UIApplication *)application
{
    ios_save_state();
}

- (void)applicationWillEnterForeground:(UIApplication *)application
{
}

- (void)applicationWillResignActive:(UIApplication *)application
{
    ios_resign_active();
}

- (void)applicationDidBecomeActive:(UIApplication *)application
{
    ios_become_active();
}

- (void)applicationWillTerminate:(UIApplication *)application
{
}

- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event
{
    if (ios_view_controller != nil && ios_view_controller.presentedViewController == nil) {
        ios_touch_began(touches);
    }
}

- (void)touchesMoved:(NSSet *)touches withEvent:(UIEvent *)event
{    
    if (ios_view_controller != nil && ios_view_controller.presentedViewController == nil) {
        ios_touch_moved(touches);
    }
}

- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event
{    
    if (ios_view_controller != nil && ios_view_controller.presentedViewController == nil) {
        ios_touch_ended(touches);
    }
}

- (void)touchesCancelled:(NSSet *)touches withEvent:(UIEvent *)event
{
    if (ios_view_controller != nil && ios_view_controller.presentedViewController == nil) {
        ios_touch_cancelled(touches);
    }
}

- (void)dealloc
{
    ios_teardown();
    [super dealloc];
    if (motionManager != nil) {
        [motionManager release];
    }
}

@end

void ios_sample_accelerometer(double *x, double *y, double *z) {
    *x = 0.0;
    *y = 0.0;
    *z = 0.0;
    if (motionManager == nil) {
        motionManager = [[CMMotionManager alloc] init];
        [[UIApplication sharedApplication] setIdleTimerDisabled: YES];
    }
    if (!motionManager.accelerometerAvailable) return;
    if (!motionManager.accelerometerActive) {
        [motionManager startAccelerometerUpdates];
    }
    CMAccelerometerData *data = motionManager.accelerometerData;
    if (data == nil) return;
    CMAcceleration accel = data.acceleration;

    *z = accel.z;

    if (has_rotated) {
        switch (last_orientation) {
            case UIInterfaceOrientationPortrait:
                *x = accel.x;
                *y = accel.y;
                break;
            case UIInterfaceOrientationPortraitUpsideDown:
                *x = -accel.x;
                *y = -accel.y;
                break;
            case UIInterfaceOrientationLandscapeLeft:
                *x = accel.y;
                *y = -accel.x;
                break;
            case UIInterfaceOrientationLandscapeRight:
                *x = -accel.y;
                *y = accel.x;
                break;
        }
    } else {
        *x = accel.x;
        *y = accel.y;
    }
}

//-------------------------------------------------------------------------------


int main(int argc, char *argv[]) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    int retVal = UIApplicationMain(argc, argv, nil, @"AMAppDelegate");
    [pool release];
    return retVal;
}

am_native_window *am_create_native_window(
    am_window_mode mode,
    am_display_orientation orientation,
    int top, int left,
    int width, int height,
    const char *title,
    bool highdpi,
    bool resizable,
    bool borderless,
    bool depth_buffer,
    bool stencil_buffer,
    int msaa_samples)
{
    if (ios_window_created) {
        am_log0("%s", "attempt to create two iOS windows");
        return NULL;
    }
    ios_window_created = true;
    ios_orientation = orientation;
    if (ios_view == nil) {
        am_log0("%s", "ios_view not initialized!");
        return NULL;
    }
    if (!am_gl_is_initialized()) {
        am_init_gl();
    }
    if (depth_buffer) {
        ios_view.drawableDepthFormat = GLKViewDrawableDepthFormat24;
    }
    if (stencil_buffer) {
        ios_view.drawableStencilFormat = GLKViewDrawableStencilFormat8;
    }
    if (msaa_samples >= 4) {
        ios_view.drawableMultisample = GLKViewDrawableMultisample4X;
    }
    return (am_native_window*)ios_view;
}

void am_get_native_window_size(am_native_window *window, int *pw, int *ph, int *sw, int *sh) {
    GLKView *view = (GLKView*)window;
    CGRect bounds = view.bounds;
    float scale = view.contentScaleFactor;
    *sw = bounds.size.width;
    *sh = bounds.size.height;
    *pw = *sw * scale;
    *ph = *sh * scale;
}

bool am_set_native_window_size_and_mode(am_native_window *window, int w, int h, am_window_mode mode) {
    return false;
}

bool am_get_native_window_lock_pointer(am_native_window *window) {
    return false;
}

void am_set_native_window_lock_pointer(am_native_window *window, bool lock) {
}

void am_destroy_native_window(am_native_window *window) {
}

void am_native_window_bind_framebuffer(am_native_window *window) {
    if (ios_view != nil) {
        [ios_view bindDrawable];
    }
}

void am_native_window_swap_buffers(am_native_window *window) {
    // handled by iOS
}

double am_get_current_time() {
    uint64_t t = mach_absolute_time();
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    return (double)t * (double)timebase.numer / (double)timebase.denom / 1e9;
}

char *am_get_base_path() {
    return am_format("%s/", ios_bundle_path());
}

char *am_get_data_path() {
    NSArray *dirs = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    const char *dir = [[dirs objectAtIndex:0] UTF8String];
    return am_format("%s/", dir);
}


void *am_read_resource(const char *filename, int *len, char** errmsg) {
    *errmsg = NULL;
    return am_read_package_resource(package, filename, len, errmsg);
}

void am_copy_video_frame_to_texture() {
}

void am_capture_audio(am_audio_bus *bus) {
}

int am_next_video_capture_frame() {
    return 0;
}

// ---------------- Game Center ---------------------

static bool gamecenter_available = false;

@interface GameCenterLeaderboardViewController : GKLeaderboardViewController 
@end

@implementation GameCenterLeaderboardViewController
@end

static GameCenterLeaderboardViewController *leaderboard_view_controller = nil;

@interface GameCenterDelegate: NSObject<GKLeaderboardViewControllerDelegate>
- (void)leaderboardViewControllerDidFinish:(GKLeaderboardViewController *)viewController;
@end

static GameCenterDelegate *gamecenter_delegate = nil;

@implementation GameCenterDelegate
- (void)leaderboardViewControllerDidFinish:(GKLeaderboardViewController *)viewController {
    if (ios_view_controller != nil) {
        [ios_view_controller dismissModalViewControllerAnimated:YES];
        if (ios_view != nil && ios_eng != NULL) {
            // reset event data, because sometimes touch end events are missed
            // when the leaderboard view controller is displayed.
            am_find_window((am_native_window*)ios_view)->push(ios_eng->L);
            am_call_amulet(ios_eng->L, "_reset_window_event_data", 1, 0);
        }
    }
}
@end

@interface GameCenterSubmitter : NSObject
+(void)submitScore:(id)data;
@end

@implementation GameCenterSubmitter

+(void)submitScore:(id)data{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    double score = [[data objectForKey:@"score"] doubleValue];
    NSString* leaderboardCategory = [data objectForKey:@"category"];
    GKScore *scoreReporter = [[[GKScore alloc] initWithCategory:leaderboardCategory] autorelease];
    scoreReporter.value = (int64_t)score;
    [scoreReporter reportScoreWithCompletionHandler:^(NSError *error)
       {
           if (error) {
                NSLog(@"Game Center Score Error: %@", [error localizedDescription]);
           }
       }
    ];
    [data release];
    [pool release];
}

+(void)submitAchievement:(id)data{
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];
    NSString *name = [data objectForKey:@"achievement"];
    GKAchievement *achievement = [[[GKAchievement alloc] initWithIdentifier:name] autorelease];
    achievement.percentComplete = 100.0;
    [achievement reportAchievementWithCompletionHandler:^(NSError *error)
       {
           if (error) {
                NSLog(@"Game Center Achievement Error: %@", [error localizedDescription]);
           }
       }
    ];
    [data release];
    [pool release];
}

@end

static int init_gamecenter(lua_State *L) {
    [[GKLocalPlayer localPlayer] authenticateWithCompletionHandler:^(NSError *error) {
        if (error == nil) {
            gamecenter_available = true;
        } else {
            NSString *description = error.localizedDescription;
            NSString *reason = error.localizedFailureReason;
            am_debug("gamecenter initialisation failed: %s (%s)", description.UTF8String, reason.UTF8String);
            gamecenter_available = false;
        }
    }];

    /*
    if ([GKLocalPlayer localPlayer].authenticated == NO) {
        GKLocalPlayer *localPlayer = [GKLocalPlayer localPlayer];
        [localPlayer setAuthenticateHandler:(^(UIViewController* viewcontroller, NSError *error) {
            if(localPlayer.isAuthenticated) {
                am_debug("%s", "gamecenter authenticated ok");
            }else {
                am_debug("%s", "gamecenter authentication failed");
            }
        })]; 
    } else {
        NSLog(@"Already authenticated!");   
    }
    */

    gamecenter_delegate = [[GameCenterDelegate alloc] init];
    [gamecenter_delegate retain];

    return 0;
}

/*
static int destroy_gamecenter(lua_State *L) {
    //fprintf(stderr, "gamecenter_delegate ref_count = %d\n", [gamecenter_delegate retain_count]);
    [gamecenter_delegate release];
    if (leaderboard_view_controller != nil) {
        //fprintf(stderr, "leaderboard_view_controller ref_count = %d\n", [leaderboard_view_controller retain_count]);
        [leaderboard_view_controller release];
    }
    return 0;
}
*/

static int submit_gamecenter_score(lua_State *L) {
    am_check_nargs(L, 2);
    const char *leaderboard = lua_tostring(L, 1);
    if (leaderboard == NULL) {
        return luaL_error(L, "expecting a string in position 1");
    }
    double score = lua_tonumber(L, 2);
    if (gamecenter_available) {
        NSDictionary* data = [[NSDictionary alloc]
            initWithObjectsAndKeys:[NSNumber numberWithDouble:score], @"score",
            [NSString stringWithUTF8String:leaderboard], @"category", nil];
        [NSThread detachNewThreadSelector:@selector(submitScore:) toTarget:[GameCenterSubmitter class] withObject:data];
    }
    return 0;
}

static int submit_gamecenter_achievement(lua_State *L) {
    am_check_nargs(L, 1);
    const char *achievement = lua_tostring(L, 1);
    if (achievement == NULL) {
        return luaL_error(L, "expecting a string in position 1");
    }
    if (gamecenter_available) {
        NSDictionary* data = [[NSDictionary alloc]
            initWithObjectsAndKeys:
            [NSString stringWithUTF8String:achievement], @"achievement", nil];
        [NSThread detachNewThreadSelector:@selector(submitAchievement:) toTarget:[GameCenterSubmitter class] withObject:data];
    }
    return 0;
}

static int show_gamecenter_leaderboard(lua_State *L) {
    am_check_nargs(L, 1);
    const char *leaderboard = lua_tostring(L, 1);
    if (leaderboard == NULL) {
        return luaL_error(L, "expecting a string in position 1");
    }
    //am_debug("showing game center leaderboard %s (%s)", leaderboard, gamecenter_available ? "available" : "not available");
    if (gamecenter_available && ios_view_controller != nil) {
        NSString *category = [NSString stringWithUTF8String:leaderboard];
        if (leaderboard_view_controller == nil) {
            leaderboard_view_controller = [[GameCenterLeaderboardViewController alloc] init];
            [leaderboard_view_controller retain];
            leaderboard_view_controller.leaderboardDelegate = gamecenter_delegate;
        }
        leaderboard_view_controller.category = category;
        [ios_view_controller presentModalViewController: leaderboard_view_controller animated: YES];
    }
    return 0;
}

static int gamecenter_is_available(lua_State *L) {
    lua_pushboolean(L, gamecenter_available);
    return 1;
}

const char *am_preferred_language() {
    NSString *languageID = [[NSBundle mainBundle] preferredLocalizations].firstObject;
    return [languageID UTF8String];
}

void am_open_ios_module(lua_State *L) {
    luaL_Reg funcs[] = {
        {"init_gamecenter", init_gamecenter},
        {"gamecenter_available", gamecenter_is_available},
        {"submit_gamecenter_score", submit_gamecenter_score},
        {"submit_gamecenter_achievement", submit_gamecenter_achievement},
        {"show_gamecenter_leaderboard", show_gamecenter_leaderboard},
        {"ios_store_pref", ios_store_pref},
        {"ios_retrieve_pref", ios_retrieve_pref},
        //{"destroy_gamecenter", destroy_gamecenter},
        {NULL, NULL}
    };
    am_open_module(L, AMULET_LUA_MODULE_NAME, funcs);
}

#endif // AM_BACKEND_IOS

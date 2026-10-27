/* Native graphics bring-up probe for the castleminerZ-nx port.
 *
 * Purpose: prove the SDL2 -> EGL -> switch-mesa GLES 3.0 -> FNA3D path on
 * physical hardware without involving Mono or any game code.
 *
 * The probe does not use the libnx console: SDL and the console both configure
 * the default NWindow, and tearing the console down between them crashes the
 * Eden test environment. Progress goes to sdmc:/switch/castleminerz-logs/
 * instead, which is readable from a desktop or from the Switch's SD card.
 *
 * This file is project-authored and contains no CastleMiner Z content.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <switch.h>

#include <SDL2/SDL.h>
#include <FNA3D.h>

static PadState pad;
static FILE *log_file = NULL;

static void probe_log(const char *format, ...)
{
    va_list args;

    if (log_file == NULL)
    {
        return;
    }

    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);
    fputc('\n', log_file);
    fflush(log_file);
}

static void log_info(const char *msg) { probe_log("[fna3d] %s", msg); }
static void log_warn(const char *msg) { probe_log("[fna3d warn] %s", msg); }
static void log_error(const char *msg) { probe_log("[fna3d error] %s", msg); }

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/castleminerz-logs", 0777);
    log_file = fopen("sdmc:/switch/castleminerz-logs/graphics-probe.log", "w");

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad);

    probe_log("CastleMiner Z NX graphics probe");
    probe_log("Native path: SDL2 -> EGL -> switch-mesa -> FNA3D (GLES3)");

    SDL_version linked;
    SDL_GetVersion(&linked);
    probe_log("SDL: %d.%d.%d", linked.major, linked.minor, linked.patch);

    FNA3D_HookLogFunctions(log_info, log_warn, log_error);
    probe_log("FNA3D linked version: %u", FNA3D_LinkedVersion());

    SDL_SetHint("FNA3D_FORCE_DRIVER", "OpenGL");
    SDL_SetHint("FNA3D_OPENGL_FORCE_ES3", "1");
    SDL_SetHint("FNA3D_OPENGL_WINDOW_DEPTHSTENCILFORMAT", "Depth24Stencil8");

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0)
    {
        probe_log("SDL_Init failed: %s", SDL_GetError());
        SDL_Quit();
        probe_log("Probe failed.");
        fclose(log_file);
        return 1;
    }

    probe_log("SDL platform: %s", SDL_GetPlatform());
    probe_log("SDL video driver: %s", SDL_GetCurrentVideoDriver());

    Uint32 windowFlags = FNA3D_PrepareWindowAttributes();
    probe_log("FNA3D window flags: 0x%08x", (unsigned int)windowFlags);

    SDL_Window *window = SDL_CreateWindow(
        "CastleMiner Z graphics probe",
        SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED,
        1280,
        720,
        windowFlags);

    if (window == NULL)
    {
        probe_log("SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        probe_log("Probe failed.");
        fclose(log_file);
        return 1;
    }

    SDL_GLContext context = SDL_GL_CreateContext(window);
    if (context == NULL)
    {
        probe_log("SDL_GL_CreateContext failed: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        probe_log("Probe failed.");
        fclose(log_file);
        return 1;
    }

    probe_log("GL context created.");

    typedef const unsigned char *(*glGetStringFn)(unsigned int);
    glGetStringFn getString =
        (glGetStringFn)SDL_GL_GetProcAddress("glGetString");

    if (getString != NULL)
    {
        probe_log("GL vendor:   %s", getString(0x1F00)); /* GL_VENDOR */
        probe_log("GL renderer: %s", getString(0x1F01)); /* GL_RENDERER */
        probe_log("GL version:  %s", getString(0x1F02)); /* GL_VERSION */
    }
    else
    {
        probe_log("glGetString was not resolvable through EGL.");
    }

    FNA3D_PresentationParameters parameters;
    memset(&parameters, 0, sizeof(parameters));
    parameters.backBufferWidth = 1280;
    parameters.backBufferHeight = 720;
    parameters.backBufferFormat = FNA3D_SURFACEFORMAT_COLOR;
    parameters.multiSampleCount = 0;
    parameters.deviceWindowHandle = window;
    parameters.isFullScreen = 1;
    parameters.depthStencilFormat = FNA3D_DEPTHFORMAT_D24S8;
    parameters.presentationInterval = FNA3D_PRESENTINTERVAL_ONE;
    parameters.displayOrientation = FNA3D_DISPLAYORIENTATION_DEFAULT;
    parameters.renderTargetUsage = FNA3D_RENDERTARGETUSAGE_DISCARDCONTENTS;

    FNA3D_Device *device = FNA3D_CreateDevice(&parameters, 0);
    if (device == NULL)
    {
        probe_log("FNA3D_CreateDevice failed for the OpenGL driver.");
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        probe_log("Probe failed.");
        fclose(log_file);
        return 1;
    }

    probe_log("FNA3D device created.");
    probe_log("Entering the clear/present loop (300 frames).");

    int frames = 0;
    int running = 1;
    while (running && frames < 300 && appletMainLoop())
    {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
        {
            running = 0;
        }

        float phase = (float)frames * 0.02f;
        FNA3D_Vec4 color;
        color.x = 0.5f + 0.5f * SDL_sinf(phase);
        color.y = 0.5f + 0.5f * SDL_sinf(phase + 2.0943951f);
        color.z = 0.5f + 0.5f * SDL_sinf(phase + 4.1887902f);
        color.w = 1.0f;

        FNA3D_Clear(
            device,
            (FNA3D_ClearOptions)(FNA3D_CLEAROPTIONS_TARGET |
                                 FNA3D_CLEAROPTIONS_DEPTHBUFFER),
            &color,
            1.0f,
            0);

        FNA3D_SwapBuffers(device, NULL, NULL, window);
        frames += 1;

        if ((frames % 60) == 0)
        {
            probe_log("presented %d frames", frames);
        }
    }

    FNA3D_DestroyDevice(device);
    SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    probe_log("Graphics probe finished; presented %d frames.", frames);

    fclose(log_file);
    return 0;
}

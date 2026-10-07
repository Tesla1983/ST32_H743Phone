/* ===========================================================================
 * 无头宿主后端 —— 代替 SDL_LCD.c，不依赖 SDL2
 *
 * 为什么需要它：计划书 §7.1 要求"先在 PC 上把 phone_shell 跑起来"，但本机
 * **没有 SDL2 开发包**。而 phone_shell 对 SDL 的依赖极薄 —— 只用到 5 个符号
 * （SDL_LCD_Init / SetTitle / PumpEvents / Delay / Destroy），且 main 在
 * SDL_LCD_Init 之后立刻把 disp->flush_cb 换成 phone_flush（自己攒全帧）。
 * 所以只要在本文件里提供这 5 个符号，就能把同一份 phone_shell 源码在宿主上
 * 编译运行，零硬件风险。
 *
 * 比 SDL 的 dummy 视频驱动更强的一点：dummy 驱动**看不到画面**，
 * 而这里把 flush_cb 的每条 band 累积成整帧，可以随时导出原始 RGB565 文件
 * （交给 tools/raw565_to_png.py 转 PNG），于是"界面长什么样"是真的可目视的。
 *
 * 另两个必需的小动作：
 *   1. SDL_LCD_Delay() 里调 YMGUI_Inject_Tick(ms)。宿主上没有别人喂 tick，
 *      不喂的话 ctx->tick_elapsed 恒为 0，启动动画（boot 800ms）永远推进不到桌面。
 *   2. PumpEvents() 里执行脚本化的指针注入（HOST_SCRIPT），用来驱动交互。
 *
 * 环境变量：
 *   HOST_SCRIPT  脚本路径，每行 "<pump 序号> <动作> [参数...]"，
 *                动作：down x y / up x y / move x y / shot 名字 / quit
 *   HOST_OUT     shot 输出目录（默认当前目录）
 * =========================================================================== */

#include "SDL_LCD.h"
#include "YMGUI_Hal.h"
#include "YMGUI_PubType.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HOST_LCD_W
#define HOST_LCD_W 320
#endif
#ifndef HOST_LCD_H
#define HOST_LCD_H 480
#endif

static GYpx s_frame[HOST_LCD_W * HOST_LCD_H];
static int  s_pump;
static int  s_quit;

#define HOST_MAX_CMD 256

typedef struct
{
    int  pump;
    char cmd[12];
    int  a;
    int  b;
    char name[64];
} HostCmd;

static HostCmd s_cmds[HOST_MAX_CMD];
static int     s_ncmd;
static int     s_loaded;

static void load_script(void)
{
    const char *path = getenv("HOST_SCRIPT");
    FILE       *f;
    char        line[256];

    if ((path == NULL) || (path[0] == '\0'))
    {
        return;
    }
    f = fopen(path, "r");
    if (f == NULL)
    {
        fprintf(stderr, "[host] script not found: %s\n", path);
        return;
    }
    while ((s_ncmd < HOST_MAX_CMD) && (fgets(line, (int)sizeof(line), f) != NULL))
    {
        HostCmd *c = &s_cmds[s_ncmd];
        int      off = 0;

        if ((line[0] == '#') || (line[0] == '\n') || (line[0] == '\r'))
        {
            continue;
        }
        memset(c, 0, sizeof(*c));

        /* 先只取"帧号 + 动作"，余下部分按动作各自解析。
         * （早先一把梭写成 "%d %s %d %d %s"，遇到 shot 的非数字参数时
         *   %d 会失败并中止，导致文件名根本没被填 —— 截图全写到了 out/.raw565） */
        if (sscanf(line, "%d %11s%n", &c->pump, c->cmd, &off) < 2)
        {
            continue;
        }
        if (strcmp(c->cmd, "shot") == 0)
        {
            if (sscanf(line + off, " %63s", c->name) != 1)
            {
                continue;
            }
        }
        else if (strcmp(c->cmd, "quit") == 0)
        {
            /* 无参数 */
        }
        else
        {
            if (sscanf(line + off, " %d %d", &c->a, &c->b) != 2)
            {
                continue;
            }
        }
        s_ncmd++;
    }
    fclose(f);
    fprintf(stderr, "[host] loaded %d script command(s)\n", s_ncmd);
}

static void dump_raw565(const char *name)
{
    char  path[512];
    FILE *f;
    const char *out = getenv("HOST_OUT");

    snprintf(path, sizeof(path), "%s/%s.raw565", (out != NULL) ? out : ".", name);
    f = fopen(path, "wb");
    if (f == NULL)
    {
        fprintf(stderr, "[host] cannot write %s\n", path);
        return;
    }
    fwrite(s_frame, sizeof(GYpx), (size_t)HOST_LCD_W * (size_t)HOST_LCD_H, f);
    fclose(f);
    fprintf(stderr, "[host] shot -> %s (%dx%d)\n", path, HOST_LCD_W, HOST_LCD_H);
}

/* flush_cb：把每条 band 贴进整帧缓冲（与板端 ymgui_port.c 的镜像缓冲同思路） */
static void host_flush(GYdisp *disp, const GYrect *area, const GYpx *pixels)
{
    int y;
    int x;

    (void)disp;
    if ((area == NULL) || (pixels == NULL))
    {
        return;
    }
    for (y = 0; y < area->h; y++)
    {
        int fy = area->y + y;

        if ((fy < 0) || (fy >= HOST_LCD_H))
        {
            continue;
        }
        for (x = 0; x < area->w; x++)
        {
            int fx = area->x + x;

            if ((fx < 0) || (fx >= HOST_LCD_W))
            {
                continue;
            }
            s_frame[(fy * HOST_LCD_W) + fx] = pixels[(y * area->w) + x];
        }
    }
}

/* ===================== SDL_LCD_* 的替代实现 ===================== */

int SDL_LCD_Init(GYDISP disp, int scale)
{
    (void)scale;
    memset(s_frame, 0, sizeof(s_frame));
    disp->flush_cb = host_flush;
    s_pump = 0;
    s_quit = 0;
    return 0;
}

void SDL_LCD_SetCloseRequestCb(SDL_LCD_CloseRequestCb cb, void *user)
{
    (void)cb;
    (void)user;
}

unsigned SDL_LCD_WindowId(void)
{
    return 1u;
}

int SDL_LCD_SetTitle(const char *title)
{
    (void)title;
    return 1;
}

void SDL_LCD_Destroy(void)
{
}

void SDL_LCD_Delay(int ms)
{
    /* ★关键★ 宿主上没有别的东西推进 YMGUI 的时基。不喂 tick 的话
     * ctx->tick_elapsed 恒为 0，phone_shell 的 advance() 拿不到时间，
     * 启动动画（phase_elapsed >= 800）永远走不完，界面停在开机页。 */
    if (ms > 0)
    {
        YMGUI_Inject_Tick((uint32)ms);
    }
}

int SDL_LCD_PumpEvents(void)
{
    int i;

    if (s_loaded == 0)
    {
        load_script();
        s_loaded = 1;
    }

    for (i = 0; i < s_ncmd; i++)
    {
        HostCmd *c = &s_cmds[i];

        if (c->pump != s_pump)
        {
            continue;
        }
        if (strcmp(c->cmd, "down") == 0)
        {
            YMGUI_Inject_Pointer((GYcoord)c->a, (GYcoord)c->b, 1);
        }
        else if (strcmp(c->cmd, "up") == 0)
        {
            YMGUI_Inject_Pointer((GYcoord)c->a, (GYcoord)c->b, 0);
        }
        else if (strcmp(c->cmd, "move") == 0)
        {
            YMGUI_Inject_Pointer((GYcoord)c->a, (GYcoord)c->b, 1);
        }
        else if (strcmp(c->cmd, "shot") == 0)
        {
            dump_raw565(c->name);
        }
        else if (strcmp(c->cmd, "quit") == 0)
        {
            s_quit = 1;
        }
        else
        {
            fprintf(stderr, "[host] unknown command: %s\n", c->cmd);
        }
    }
    s_pump++;

    return (s_quit != 0) ? 0 : 1;
}

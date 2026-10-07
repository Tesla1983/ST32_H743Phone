/**
 ****************************************************************************************************
 * @file        lcd.c
 * @version     V1.0
 * @brief       2.8寸/3.5寸/4.3寸 TFTLCD(MCU屏) 驱动代码	
 *              支持驱动IC型号包括:NT35310/NT35510/ST7796/ST7789/ILI9806
 ****************************************************************************************************
 * @attention   Waiken-Smart 慧勤智远
 *
 * 实验平台:    STM32H743VIT6小系统板
 *
 ****************************************************************************************************
 */
 
#include "stdlib.h"
#include "./BSP/LCD/lcd.h"
#include "./BSP/LCD/lcdfont.h"
#include "./SYSTEM/usart/usart.h"


/* lcd_ex.c存放各个LCD驱动IC的寄存器初始化部分代码,以简化lcd.c,该.c文件
 * 不直接加入到工程里面,只有lcd.c会用到,所以通过include的形式添加.(不要在
 * 其他文件再包含该.c文件!!否则会报错!)
 */
#include "./BSP/LCD/lcd_ex.c"

SRAM_HandleTypeDef g_sram_handle;       /* SRAM句柄(用于控制LCD) */

/* LCD的画笔颜色和背景色 */
uint32_t g_point_color = 0XF800;        /* 画笔颜色 */
uint32_t g_back_color  = 0XFFFF;        /* 背景色 */

/* 管理LCD重要参数 */
_lcd_dev lcddev;


/**
 * @brief       LCD写数据
 * @param       data: 要写入的数据
 * @retval      无
 */
void lcd_wr_data(volatile uint16_t data)
{
    data = data;            /* 使用-O2优化的时候,必须插入的延时 */
    LCD->LCD_RAM = data;
}

/**
 * @brief       LCD写寄存器编号/地址函数
 * @param       regno: 寄存器编号/地址
 * @retval      无
 */
void lcd_wr_regno(volatile uint16_t regno)
{
    regno = regno;          /* 使用-O2优化的时候,必须插入的延时 */
    LCD->LCD_REG = regno;   /* 写入要写的寄存器序号 */
}

/**
 * @brief       LCD写寄存器
 * @param       regno:寄存器编号/地址
 * @param       data:要写入的数据
 * @retval      无
 */
void lcd_write_reg(uint16_t regno, uint16_t data)
{
    LCD->LCD_REG = regno;   /* 写入要写的寄存器序号 */
    LCD->LCD_RAM = data;    /* 写入数据 */
}

/**
 * @brief       LCD读数据
 * @param       无
 * @retval      读取到的数据
 */
static uint16_t lcd_rd_data(void)
{
    volatile uint16_t ram;  /* 防止被优化 */
    ram = LCD->LCD_RAM;
    return ram;
}

/**
 * @brief       LCD延时函数,仅用于部分在mdk -O1时间优化时需要设置的地方
 * @param       i:延时的数值
 * @retval      无
 */
static void lcd_opt_delay(uint32_t i)
{
    while (i--);            /* 使用AC6时空循环可能被优化,可使用while(1) __asm volatile(""); */
}

/**
 * @brief       准备写GRAM
 * @param       无
 * @retval      无
 */
void lcd_write_ram_prepare(void)
{
    LCD->LCD_REG = lcddev.wramcmd;
}

/**
 * @brief       读取某个点的颜色值
 * @param       x,y:坐标
 * @retval      此点的颜色
 */
uint32_t lcd_read_point(uint16_t x, uint16_t y)
{
    uint16_t r = 0, g = 0, b = 0;

    if (x >= lcddev.width || y >= lcddev.height)
    {
        return 0;               /* 超过了范围,直接返回 */
    }
    
    lcd_set_cursor(x, y);       /* 设置坐标 */

    if (lcddev.id == 0x5510)
    {
        lcd_wr_regno(0x2E00);   /* 5510 发送读GRAM指令 */
    }
    else
    {
        lcd_wr_regno(0x2E);     /* 其他IC(7796/5310/7789/9806)发送读GRAM指令 */
    }

    r = lcd_rd_data();          /* 假读(dummy read) */

    lcd_opt_delay(2);
    r = lcd_rd_data();          /* 实际坐标颜色 */
    
    if (lcddev.id == 0x7796)    /* 7796 一次读取一个像素值 */
    {
        return r;
    }
    
    /* 5310/5510/7789/9806 要分2次读出 */
    lcd_opt_delay(2);
    b = lcd_rd_data();
    g = r & 0XFF;               /* 对于 5310/5510/7789/9806, 第一次读取的是RG的值,R在前,G在后,各占8位 */
    g <<= 8;
    
    return (((r >> 11) << 11) | ((g >> 10) << 5) | (b >> 11));  /* 5310/5510/7789/9806 需要公式转换一下 */
}

/**
 * @brief       LCD开启显示
 * @param       无
 * @retval      无
 */
void lcd_display_on(void)
{
    if (lcddev.id == 0X5510)    /* 5510开启显示指令 */
    {
        lcd_wr_regno(0X2900);   /* 开启显示 */
    }
    else                        /* 5310/7789/7796/9806 等发送开启显示指令 */
    {
        lcd_wr_regno(0X29);     /* 开启显示 */
    }
}

/**
 * @brief       LCD关闭显示
 * @param       无
 * @retval      无
 */
void lcd_display_off(void)
{
    if (lcddev.id == 0X5510)    /* 5510关闭显示指令 */
    {
        lcd_wr_regno(0X2800);   /* 关闭显示 */
    }
    else                        /* 5310/7789/7796/9806 等发送关闭显示指令 */
    {
        lcd_wr_regno(0X28);     /* 关闭显示 */
    }
}

/**
 * @brief       设置光标位置
 * @param       x,y: 坐标
 * @retval      无
 */
void lcd_set_cursor(uint16_t x, uint16_t y)
{
    if (lcddev.id == 0X5510)  /* 5510设置坐标 */
    {
        lcd_wr_regno(lcddev.setxcmd);
        lcd_wr_data(x >> 8);
        lcd_wr_regno(lcddev.setxcmd + 1);
        lcd_wr_data(x & 0XFF);
        lcd_wr_regno(lcddev.setycmd);
        lcd_wr_data(y >> 8);
        lcd_wr_regno(lcddev.setycmd + 1);
        lcd_wr_data(y & 0XFF);
    }
    else                      /* 5310/7789/7796/9806设置坐标 */
    {
        lcd_wr_regno(lcddev.setxcmd);
        lcd_wr_data(x >> 8);
        lcd_wr_data(x & 0XFF);
        lcd_wr_regno(lcddev.setycmd);
        lcd_wr_data(y >> 8);
        lcd_wr_data(y & 0XFF);
    }
}

/**
 * @brief       设置LCD的自动扫描方向
 * @note
 *              注意:其他函数可能会受到此函数设置的影响,
 *              所以,一般设置为L2R_U2D即可,如果设置为其他扫描方式,可能导致显示不正常.
 *
 * @param       dir:0~7,代表8个方向(具体定义见lcd.h)
 * @retval      无
 */
void lcd_scan_dir(uint8_t dir)
{
    uint16_t regval = 0;
    uint16_t dirreg = 0;
    uint16_t temp;

    /* 横屏时，IC改变扫描方向！竖屏时, IC不改变扫描方向 */
    if (lcddev.dir == 1)
    {
        switch (dir)   /* 方向转换 */
        {
            case 0:
                dir = 6;
                break;

            case 1:
                dir = 7;
                break;

            case 2:
                dir = 4;
                break;

            case 3:
                dir = 5;
                break;

            case 4:
                dir = 1;
                break;

            case 5:
                dir = 0;
                break;

            case 6:
                dir = 3;
                break;

            case 7:
                dir = 2;
                break;
        }
    }
 
    /* 根据扫描方式 设置 0X36/0X3600 寄存器 bit 5,6,7 位的值 */
    switch (dir)
    {
        case L2R_U2D:      /* 从左到右,从上到下 */
            regval |= (0 << 7) | (0 << 6) | (0 << 5);
            break;

        case L2R_D2U:      /* 从左到右,从下到上 */
            regval |= (1 << 7) | (0 << 6) | (0 << 5);
            break;

        case R2L_U2D:      /* 从右到左,从上到下 */
            regval |= (0 << 7) | (1 << 6) | (0 << 5);
            break;

        case R2L_D2U:      /* 从右到左,从下到上 */
            regval |= (1 << 7) | (1 << 6) | (0 << 5);
            break;

        case U2D_L2R:      /* 从上到下,从左到右 */
            regval |= (0 << 7) | (0 << 6) | (1 << 5);
            break;

        case U2D_R2L:      /* 从上到下,从右到左 */
            regval |= (0 << 7) | (1 << 6) | (1 << 5);
            break;

        case D2U_L2R:      /* 从下到上,从左到右 */
            regval |= (1 << 7) | (0 << 6) | (1 << 5);
            break;

        case D2U_R2L:      /* 从下到上,从右到左 */
            regval |= (1 << 7) | (1 << 6) | (1 << 5);
            break;
    }

    dirreg = 0X36;         /* 对绝大部分驱动IC, 由0X36寄存器控制 */

    if (lcddev.id == 0X5510)
    {
        dirreg = 0X3600;   /* 对于5510, 和其他驱动IC的寄存器有差异 */
    }

    /* 7789 & 7796 要设置BGR位 */
    if (lcddev.id == 0X7789 || lcddev.id == 0X7796)
    {
        regval |= 0X08;
    }

    lcd_write_reg(dirreg, regval);

    if (regval & 0X20)
    {
        if (lcddev.width < lcddev.height)   /* 交换X,Y */
        {
            temp = lcddev.width;
            lcddev.width = lcddev.height;
            lcddev.height = temp;
        }
    }
    else
    {
        if (lcddev.width > lcddev.height)   /* 交换X,Y */
        {
            temp = lcddev.width;
            lcddev.width = lcddev.height;
            lcddev.height = temp;
        }
    }

    /* 设置显示区域(开窗)大小 */
    if (lcddev.id == 0X5510)
    {
        lcd_wr_regno(lcddev.setxcmd);
        lcd_wr_data(0);
        lcd_wr_regno(lcddev.setxcmd + 1);
        lcd_wr_data(0);
        lcd_wr_regno(lcddev.setxcmd + 2);
        lcd_wr_data((lcddev.width - 1) >> 8);
        lcd_wr_regno(lcddev.setxcmd + 3);
        lcd_wr_data((lcddev.width - 1) & 0XFF);
        lcd_wr_regno(lcddev.setycmd);
        lcd_wr_data(0);
        lcd_wr_regno(lcddev.setycmd + 1);
        lcd_wr_data(0);
        lcd_wr_regno(lcddev.setycmd + 2);
        lcd_wr_data((lcddev.height - 1) >> 8);
        lcd_wr_regno(lcddev.setycmd + 3);
        lcd_wr_data((lcddev.height - 1) & 0XFF);
    }
    else
    {
        lcd_wr_regno(lcddev.setxcmd);
        lcd_wr_data(0);
        lcd_wr_data(0);
        lcd_wr_data((lcddev.width - 1) >> 8);
        lcd_wr_data((lcddev.width - 1) & 0XFF);
        lcd_wr_regno(lcddev.setycmd);
        lcd_wr_data(0);
        lcd_wr_data(0);
        lcd_wr_data((lcddev.height - 1) >> 8);
        lcd_wr_data((lcddev.height - 1) & 0XFF);
    }
}

/**
 * @brief       画点
 * @param       x,y: 坐标
 * @param       color: 点的颜色
 * @retval      无
 */
void lcd_draw_point(uint16_t x, uint16_t y, uint32_t color)
{    
    lcd_set_cursor(x, y);       /* 设置光标位置 */
    lcd_write_ram_prepare();    /* 开始写入GRAM */
    LCD->LCD_RAM = color;
}

/**
 * @brief       设置LCD显示方向
 * @param       dir:0,竖屏; 1,横屏
 * @retval      无
 */
void lcd_display_dir(uint8_t dir)
{
    lcddev.dir = dir;   /* 竖屏/横屏 */
    
    if (dir == 0)       /* 竖屏 */
    {
        lcddev.width = 240;
        lcddev.height = 320;

        if (lcddev.id == 0x5510)
        {
            lcddev.wramcmd = 0X2C00;  /* 设置写入GRAM的指令 */
            lcddev.setxcmd = 0X2A00;  /* 设置写X坐标指令 */
            lcddev.setycmd = 0X2B00;  /* 设置写Y坐标指令 */
            lcddev.width = 480;       /* 设置宽度480 */
            lcddev.height = 800;      /* 设置高度800 */
        }
        else   /* 其他IC, 包括: 5310/7789/7796/9806等IC */
        {
            lcddev.wramcmd = 0X2C;
            lcddev.setxcmd = 0X2A;
            lcddev.setycmd = 0X2B;
        }

        if (lcddev.id == 0X5310 || lcddev.id == 0X7796)    /* 如果是5310/7796 则表示是 320*480分辨率 */
        {
            lcddev.width = 320;
            lcddev.height = 480;
        }
        
        if (lcddev.id == 0X9806)    /* 如果是9806 则表示是 480*800分辨率 */
        {
            lcddev.width = 480;
            lcddev.height = 800;
        }    
    }
    else                /* 横屏 */
    {
        lcddev.width = 320;         /* 默认宽度 */
        lcddev.height = 240;        /* 默认高度 */

        if (lcddev.id == 0x5510)
        {
            lcddev.wramcmd = 0X2C00;  /* 设置写入GRAM的指令 */
            lcddev.setxcmd = 0X2A00;  /* 设置写X坐标指令 */
            lcddev.setycmd = 0X2B00;  /* 设置写Y坐标指令 */
            lcddev.width = 800;       /* 设置宽度800 */
            lcddev.height = 480;      /* 设置高度480 */
        }
        else   /* 其他IC, 包括: 5310/7789/7796/9806等IC */
        {
            lcddev.wramcmd = 0X2C;
            lcddev.setxcmd = 0X2A;
            lcddev.setycmd = 0X2B;
        }

        if (lcddev.id == 0X5310 || lcddev.id == 0X7796)    /* 如果是5310/7796 则表示是 480*320分辨率 */
        {
            lcddev.width = 480;
            lcddev.height = 320;
        }
        
        if (lcddev.id == 0X9806)    /* 如果是9806 则表示是 800*480分辨率 */
        {
            lcddev.width = 800;
            lcddev.height = 480;
        }   
    }

    lcd_scan_dir(DFT_SCAN_DIR);     /* 默认扫描方向 */
}

/**
 * @brief       设置窗口,并自动设置画点坐标到窗口左上角(sx,sy).
 * @param       sx,sy:窗口起始坐标(左上角)
 * @param       width,height:窗口宽度和高度,必须大于0!!
 * @note        窗体大小:width*height.
 *
 * @retval      无
 */
void lcd_set_window(uint16_t sx, uint16_t sy, uint16_t width, uint16_t height)
{
    uint16_t twidth, theight;
    twidth = sx + width - 1;
    theight = sy + height - 1;

    if (lcddev.id == 0X5510)     /* 5510设置窗口 */
    {
        lcd_wr_regno(lcddev.setxcmd);
        lcd_wr_data(sx >> 8);
        lcd_wr_regno(lcddev.setxcmd + 1);
        lcd_wr_data(sx & 0XFF);
        lcd_wr_regno(lcddev.setxcmd + 2);
        lcd_wr_data(twidth >> 8);
        lcd_wr_regno(lcddev.setxcmd + 3);
        lcd_wr_data(twidth & 0XFF);
        lcd_wr_regno(lcddev.setycmd);
        lcd_wr_data(sy >> 8);
        lcd_wr_regno(lcddev.setycmd + 1);
        lcd_wr_data(sy & 0XFF);
        lcd_wr_regno(lcddev.setycmd + 2);
        lcd_wr_data(theight >> 8);
        lcd_wr_regno(lcddev.setycmd + 3);
        lcd_wr_data(theight & 0XFF);
    }
    else    /* 5310/7789/7796/9806设置窗口 */
    {
        lcd_wr_regno(lcddev.setxcmd);
        lcd_wr_data(sx >> 8);
        lcd_wr_data(sx & 0XFF);
        lcd_wr_data(twidth >> 8);
        lcd_wr_data(twidth & 0XFF);
        lcd_wr_regno(lcddev.setycmd);
        lcd_wr_data(sy >> 8);
        lcd_wr_data(sy & 0XFF);
        lcd_wr_data(theight >> 8);
        lcd_wr_data(theight & 0XFF);
    }
}

/**
 * @brief       SRAM底层驱动，时钟使能，引脚分配
 * @note        此函数会被HAL_SRAM_Init()调用,初始化读写总线引脚
 * @param       hsram:SRAM句柄
 * @retval      无
 */
void HAL_SRAM_MspInit(SRAM_HandleTypeDef *hsram)
{
    GPIO_InitTypeDef gpio_init_struct;

    __HAL_RCC_FMC_CLK_ENABLE();                         /* 使能FMC时钟 */
    __HAL_RCC_GPIOD_CLK_ENABLE();                       /* 使能GPIOD时钟 */
    __HAL_RCC_GPIOE_CLK_ENABLE();                       /* 使能GPIOE时钟 */

    /* 初始化PD0,1,8,9,10,14,15 */
    gpio_init_struct.Pin = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10| \
                           GPIO_PIN_14 | GPIO_PIN_15;
    gpio_init_struct.Mode = GPIO_MODE_AF_PP;            /* 复用推挽 */
    gpio_init_struct.Pull = GPIO_PULLUP;                /* 上拉 */
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_VERY_HIGH; /* 高速 */
    gpio_init_struct.Alternate = GPIO_AF12_FMC;         /* 复用为FMC */
    HAL_GPIO_Init(GPIOD, &gpio_init_struct);            /* 初始化IO口 */

    /* 初始化PE7,8,9,10,11,12,13,14,15 */
    gpio_init_struct.Pin = GPIO_PIN_7 | GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10 \
                           | GPIO_PIN_11 | GPIO_PIN_12 | GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
    HAL_GPIO_Init(GPIOE, &gpio_init_struct);            /* 初始化IO口 */
}

/**
 * @brief       初始化LCD
 * @note        该初始化函数可以初始化各种型号的LCD(详见本.c文件最前面的描述)
 * @param       无
 * @retval      无
 */
/* ===========================================================================
 * FMC 写时序：运行期可改（2026-10-04 步骤 3）
 *
 * 【为什么做成全局量】一次 16 位写的成本 = (ADDSET + DATAST + 1) / 220MHz，
 * 而 5/5 只是阶段 0 那一轮"七档整体扫描"选出来的**一个点**，不是逐维最优：
 * 只有 DATAST 受面板 tWRL ≥ 19ns 约束（≥4），ADDSET 从没被单独扫过。
 * 做成可写全局量 + 一个 apply 钩子，就能在不重新编译、不复位的前提下扫
 * (ADDSET, DATAST) 二维格点，每档读一次 g_flush_cyc（见 main.c 的
 * g_fmc_timing_sweep 钩子，它顺手把整屏 flush 也重测一遍）。
 *
 * 【单位】fmc_ker_ck 周期。本板 FMC = PLL2_R = 220MHz ⇒ 1 周期 = 4.545 ns。
 * 【判据】换档后画面可能错乱；定档必须复跑 A2 双自检，g_ramp_mismatch 与
 *         g_gram_mismatch 都要为 0，且要跑够几十帧再复测一次。
 * =========================================================================== */
/* 用 uint32 而不是 uint8：SWD 只能按 8/16/32 位粒度写，用 32 位量就能走
 * 与其它钩子完全相同的 `probe-rs write b32`，不会误伤相邻字节（库里其余
 * 运行期钩子也全是 32 位）。取值范围只有 1..15，用 32 位不浪费什么。 */
volatile uint32_t g_lcd_fmc_addset = 5;
volatile uint32_t g_lcd_fmc_datast = 5;

void lcd_fmc_write_timing_apply(void)
{
    /* ★必须 {0} 清零★ 2026-10-04 实测踩到的真坑：
     * FMC_NORSRAM_TimingTypeDef 有 7 个字段，而 FMC_NORSRAM_Extended_Timing_Init
     * 会把**全部 7 个**都写进 BWTR1（写时序寄存器）。只赋 4 个字段的话，
     * 剩下 3 个（BusTurnAroundDuration / CLKDivision / DataLatency）
     * 就是**未初始化的栈垃圾**，直接进寄存器。
     *
     * 症状：ADDSET/DATAST 明明是对的（回读 BWTR1 = 0x3FFF0402 ⇒ ADDSET=2 DATAST=4），
     * 但整屏 flush 实测 104.5 ns/px ≈ 23 个 FMC 周期，而 (2+4+1)=7 周期只有 31.8 ns
     * —— 多出来的约 15 个周期正是垃圾 BUSTURN=15 插进去的。
     * 之所以之前几轮量到"六档与理论逐档吻合"，是那次栈垃圾恰好为 0；
     * **也就是说这个 bug 会让 FMC 写时序跨启动不确定**（同一份固件有时 7 周期、
     * 有时 22 周期）。厂商 lcd_init 里的 fmc_write_handle 有同样毛病，一并修。
     *
     * 本工程是异步 SRAM 模式（BurstAccessMode=DISABLE），CLKDIV/DataLatency
     * 本来就不用，写 0 即可；BUSTURN 写 0 让连续写之间不插总线周转。 */
    FMC_NORSRAM_TimingTypeDef t = {0};

    t.AddressSetupTime         = (uint32_t)g_lcd_fmc_addset;
    t.AddressHoldTime          = 0x00;
    t.DataSetupTime            = (uint32_t)g_lcd_fmc_datast;
    t.BusTurnAroundDuration    = 0x00;   /* ← 不清零会变成 0xF，每次写多付 15 周期 */
    t.CLKDivision              = 0x00;
    t.DataLatency              = 0x00;
    t.AccessMode               = FMC_ACCESS_MODE_A;

    /* 写时序走"扩展模式"寄存器（ExtendedMode=ENABLE，读写分开）——
     * 与 lcd_init 里初始化读时序/写时序时同一套调用，所以这里必须是
     * Extended 的初始化函数，不能调 FMC_NORSRAM_Timing_Init。 */
    (void)FMC_NORSRAM_Extended_Timing_Init(g_sram_handle.Extended, &t,
                                           g_sram_handle.Init.NSBank,
                                           g_sram_handle.Init.ExtendedMode);
}

void lcd_init(void)
{
    GPIO_InitTypeDef gpio_init_struct;
    /* ★必须 {0} 清零★（2026-10-04 修）：FMC_NORSRAM_TimingTypeDef 有 7 个字段，
     * 而 HAL_SRAM_Init / FMC_NORSRAM_Timing_Init 会把**全部 7 个**写进 BTR/BWTR。
     * 厂商原码只赋了 4 个（AddressSetupTime/AddressHoldTime/DataSetupTime/AccessMode），
     * 剩下 3 个（BusTurnAroundDuration/CLKDivision/DataLatency）是**未初始化的栈垃圾**。
     * 实测代价：垃圾 BUSTURN=15 会让每次 16 位写多付约 15 个 FMC 周期
     * （整屏 flush 从 31.8 ns/px 变 104.5 ns/px，慢 3.3 倍），而且**跨启动不确定**
     * —— 同一份固件有时 7 周期、有时 22 周期，取决于那次栈上残留了什么。
     * 详见 lcd_fmc_write_timing_apply() 上方的长注释。 */
    FMC_NORSRAM_TimingTypeDef fmc_read_handle  = {0};
    FMC_NORSRAM_TimingTypeDef fmc_write_handle = {0};
  
    /* IO 及 时钟配置 */
    LCD_CS_GPIO_CLK_ENABLE();   /* LCD_CS引脚时钟使能 */
    LCD_RS_GPIO_CLK_ENABLE();   /* LCD_RS引脚时钟使能 */
    LCD_WR_GPIO_CLK_ENABLE();   /* LCD_WR引脚时钟使能 */
    LCD_RD_GPIO_CLK_ENABLE();   /* LCD_RD引脚时钟使能 */
    LCD_BL_GPIO_CLK_ENABLE();   /* LCD_BL引脚时钟使能 */
    LCD_RST_GPIO_CLK_ENABLE();  /* LCD_RST引脚时钟使能 */

    gpio_init_struct.Pin = LCD_CS_GPIO_PIN;
    gpio_init_struct.Mode = GPIO_MODE_AF_PP;                /* 复用推挽输出 */
    gpio_init_struct.Pull = GPIO_PULLUP;                    /* 上拉 */
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;     /* 高速 */
    gpio_init_struct.Alternate = GPIO_AF12_FMC;             /* 复用为FMC */
    HAL_GPIO_Init(LCD_CS_GPIO_PORT, &gpio_init_struct);     /* 初始化LCD_CS引脚 */

    gpio_init_struct.Pin = LCD_RS_GPIO_PIN;
    HAL_GPIO_Init(LCD_RS_GPIO_PORT, &gpio_init_struct);     /* 初始化LCD_RS引脚 */
    
    gpio_init_struct.Pin = LCD_WR_GPIO_PIN;
    HAL_GPIO_Init(LCD_WR_GPIO_PORT, &gpio_init_struct);     /* 初始化LCD_WR引脚 */

    gpio_init_struct.Pin = LCD_RD_GPIO_PIN;
    HAL_GPIO_Init(LCD_RD_GPIO_PORT, &gpio_init_struct);     /* 初始化LCD_RD引脚 */

    gpio_init_struct.Pin = LCD_BL_GPIO_PIN;
    gpio_init_struct.Mode = GPIO_MODE_OUTPUT_PP;            /* 推挽输出 */
    HAL_GPIO_Init(LCD_BL_GPIO_PORT, &gpio_init_struct);     /* 初始化LCD_BL引脚 */

    gpio_init_struct.Pin = LCD_RST_GPIO_PIN;
    gpio_init_struct.Mode = GPIO_MODE_OUTPUT_PP;            /* 推挽输出 */
    HAL_GPIO_Init(LCD_RST_GPIO_PORT, &gpio_init_struct);    /* 初始化LCD_RST引脚 */

    g_sram_handle.Instance = FMC_NORSRAM_DEVICE;
    g_sram_handle.Extended = FMC_NORSRAM_EXTENDED_DEVICE;
    
    g_sram_handle.Init.NSBank = FMC_NORSRAM_BANK1;                        /* 使用NE1 */
    g_sram_handle.Init.DataAddressMux = FMC_DATA_ADDRESS_MUX_DISABLE;     /* 地址/数据线不复用 */
    g_sram_handle.Init.MemoryType = FMC_MEMORY_TYPE_SRAM;                 /* 存储器类型为SRAM */
    g_sram_handle.Init.MemoryDataWidth = FMC_NORSRAM_MEM_BUS_WIDTH_16;    /* 16位数据宽度 */
    g_sram_handle.Init.BurstAccessMode = FMC_BURST_ACCESS_MODE_DISABLE;   /* 是否使能突发访问,仅对同步突发存储器有效,此处未用到 */
    g_sram_handle.Init.WaitSignalPolarity = FMC_WAIT_SIGNAL_POLARITY_LOW; /* 等待信号的极性,仅在突发模式访问下有用 */
    g_sram_handle.Init.WaitSignalActive = FMC_WAIT_TIMING_BEFORE_WS;      /* 存储器是在等待周期之前的一个时钟周期还是等待周期期间使能NWAIT */
    g_sram_handle.Init.WriteOperation = FMC_WRITE_OPERATION_ENABLE;       /* 存储器写使能 */
    g_sram_handle.Init.WaitSignal = FMC_WAIT_SIGNAL_DISABLE;              /* 等待使能位,此处未用到 */
    g_sram_handle.Init.ExtendedMode = FMC_EXTENDED_MODE_ENABLE;           /* 扩展模式使能,读写使用不同的时序 */
    g_sram_handle.Init.AsynchronousWait = FMC_ASYNCHRONOUS_WAIT_DISABLE;  /* 是否使能同步传输模式下的等待信号,此处未用到 */
    g_sram_handle.Init.WriteBurst = FMC_WRITE_BURST_DISABLE;              /* 禁止突发写 */
    g_sram_handle.Init.ContinuousClock = FMC_CONTINUOUS_CLOCK_SYNC_ASYNC; /* 是否使能连续时钟FMC_CLK输出,此处未用到 */
    
    /* FMC读时序控制寄存器 */
    fmc_read_handle.AddressSetupTime = 0x0F;          /* 地址建立时间(ADDSET)为15个fmc_ker_ck 1/220M = 4.5ns * 15 = 67.5ns */
    fmc_read_handle.AddressHoldTime = 0x00;
    fmc_read_handle.DataSetupTime = 0x4E;             /* 数据保存时间(DATAST)为78个fmc_ker_ck = 4.5 * 78 = 351ns */
                                                      /* 液晶驱动IC读数据的时候，速度不能太快 */
    fmc_read_handle.AccessMode = FMC_ACCESS_MODE_A;   /* 模式A */
    /* FMC写时序控制寄存器 */
    fmc_write_handle.AddressSetupTime = 0x0F;         /* 地址建立时间(ADDSET)为15个fmc_ker_ck = 4.5ns * 15 = 67.5ns */
    fmc_write_handle.AddressHoldTime = 0x00;
    fmc_write_handle.DataSetupTime = 0x0F;            /* 数据保存时间(DATAST)为15个fmc_ker_ck = 4.5ns * 15 = 67.5ns */
                                                      /* 某些液晶驱动IC的写信号脉宽，初始化的时候需要设置大一些 */
    fmc_write_handle.AccessMode = FMC_ACCESS_MODE_A;  /* 模式A */
    HAL_SRAM_Init(&g_sram_handle, &fmc_read_handle, &fmc_write_handle);
    delay_ms(50);

    /* LCD复位 */
	  LCD_RST(1);
	  delay_ms(10);
	  LCD_RST(0);
	  delay_ms(50);
	  LCD_RST(1); 
		delay_ms(200); 

    /* 尝试7796 ID的读取 */
    lcd_wr_regno(0XD3);
    lcddev.id = lcd_rd_data();  /* dummy read */
    lcddev.id = lcd_rd_data();  /* 读到0X00 */
    lcddev.id = lcd_rd_data();  /* 读取0X77 */
    lcddev.id <<= 8;
    lcddev.id |= lcd_rd_data(); /* 读取0X96 */

    if (lcddev.id != 0X7796)    /* 不是7796,尝试看看是不是ST7789 */
    {
        lcd_wr_regno(0X04);
        lcddev.id = lcd_rd_data();      /* dummy read */
        lcddev.id = lcd_rd_data();      /* 读到0X85 */
        lcddev.id = lcd_rd_data();      /* 读取0X85 */
        lcddev.id <<= 8;
        lcddev.id |= lcd_rd_data();     /* 读取0X52 */
        
        if (lcddev.id == 0X8552)        /* 将8552的ID转换成7789 */
        {
            lcddev.id = 0x7789;
        }

        if (lcddev.id != 0x7789)        /* 也不是ST7789,尝试是不是NT35310 */
        {
            lcd_wr_regno(0XD4);
            lcddev.id = lcd_rd_data();  /* dummy read */
            lcddev.id = lcd_rd_data();  /* 读回0X01 */
            lcddev.id = lcd_rd_data();  /* 读回0X53 */
            lcddev.id <<= 8;
            lcddev.id |= lcd_rd_data(); /* 这里读回0X10 */

            if (lcddev.id != 0X5310)    /* 也不是NT35310,尝试看看是不是NT35510 */
            {
                /* 发送秘钥（厂家提供,照搬即可） */
                lcd_write_reg(0xF000, 0x0055);
                lcd_write_reg(0xF001, 0x00AA);
                lcd_write_reg(0xF002, 0x0052);
                lcd_write_reg(0xF003, 0x0008);
                lcd_write_reg(0xF004, 0x0001);
                
                lcd_wr_regno(0xC500);           /* 读取ID高8位 */
                lcddev.id = lcd_rd_data();      /* 读回0X55 */
                lcddev.id <<= 8;

                lcd_wr_regno(0xC501);           /* 读取ID低8位 */
                lcddev.id |= lcd_rd_data();     /* 读回0X10 */   

                if (lcddev.id != 0X5510)        /* 也不是NT35510,尝试看看是不是ILI9806 */
                {
                    lcd_wr_regno(0XD3);
                    lcddev.id = lcd_rd_data();  /* dummy read */
                    lcddev.id = lcd_rd_data();  /* 读回0X00 */
                    lcddev.id = lcd_rd_data();  /* 读回0X98 */
                    lcddev.id <<= 8;
                    lcddev.id |= lcd_rd_data(); /* 读回0X06 */                 
                }                  
            }
        }
    }

    /* 特别注意, 如果在main函数里面屏蔽串口1初始化, 则会卡死在printf
     * 里面(卡死在f_putc函数), 所以, 必须初始化串口1, 或者屏蔽掉下面
     * 这行 printf 语句 !!!!!!!
     */
    /* ★移植改动（STM32H743 裸机）★ 上面那行 printf("LCD ID:%x\r\n", lcddev.id)
     * 已删除。原因：裸机上 printf 首次调用会经 newlib malloc/_sbrk 给 stdout 分配
     * 行缓冲，而原链接脚本让 _sbrk 的起点（符号 end）与 YMGUI 的 heap0 落在同一
     * 地址 0x2000_04B0，两者重合 → stdio 缓冲的写入会踩坏 YMGUI 的 ctx。
     * 详见 linker/stm32h743vit6.ld 里的说明。
     * LCD ID 仍可用 SWD 读 lcddev.id，或读 src/main.c 的 g_lcd_id。 */

    if (lcddev.id == 0X7789)
    {
        lcd_ex_st7789_reginit();        /* 执行ST7789初始化 */
    }
    else if (lcddev.id == 0x5310)
    {
        lcd_ex_nt35310_reginit();       /* 执行NT35310初始化 */
    }
    else if (lcddev.id == 0x7796)
    {
        lcd_ex_st7796_reginit();        /* 执行ST7796初始化 */
    }
    else if (lcddev.id == 0x5510)
    {
        lcd_ex_nt35510_reginit();       /* 执行NT35510初始化 */
    }
    else if (lcddev.id == 0x9806)
    {
        lcd_ex_ili9806_reginit();       /* 执行ILI9806初始化 */
    } 
    
    /* 由于不同屏幕的写时序不同，这里的时序可以根据自己的屏幕进行修改
      （若插上长排线对时序也会有影响，需要自己根据情况修改） */
    /* 初始化完成以后,提速 */
    if (lcddev.id == 0x7789)
    {
        /* 重新配置写时序控制寄存器的时序 */
        fmc_write_handle.AddressSetupTime = 5;  /* 地址建立时间(ADDSET)为5个fmc_ker_ck = 22.5 ns */
        fmc_write_handle.DataSetupTime = 5;     /* 数据保存时间(DATAST)为5个fmc_ker_ck = 22.5 ns */
        FMC_NORSRAM_Extended_Timing_Init(g_sram_handle.Extended, &fmc_write_handle, g_sram_handle.Init.NSBank, g_sram_handle.Init.ExtendedMode);
    }
    else if (lcddev.id == 0x5310 || lcddev.id == 0x7796 || lcddev.id == 0x5510 || lcddev.id == 0x9806)
    {
        /* ★移植修正（STM32H743VIT6 小系统板）★
         * 厂商原值 3/3 是按其 FMC 内核时钟 220MHz 折算的（3 周期 ≈ 13.5ns）。
         * 3 周期只有 **13.5ns**，**低于面板规格书要求的写脉宽 tWRL >= 19ns**
         * ⇒ 像素写入错位 ⇒ 整屏花屏。
         *
         * 改用阶段 0 在本板上逐档实测选定的 **5/5**（ADDSET=5 + DATAST=5）：
         * 七档扫描（15/15…4/4）全部通过「整屏 ramp 初测失配 0 + 100 帧渲染后复测失配 0」，
         * 抓屏条纹规则无花屏。依据见 E:\stm32-tetris\docs\FMC_WRITE_TIMING_SCAN.md
         *
         * ★★★ 2026-10-04 步骤 3：时序改成**运行期可扫** ★★★
         *
         * 【上一版注释里的时钟写错了】这里原来写"本板 FMC = HCLK3 = 200MHz"，
         * 但 sys.c 实际是 `PLL2M=25, PLL2N=440, PLL2R=2` + `FmcClockSelection =
         * RCC_FMCCLKSOURCE_PLL2` ⇒ **FMC 内核时钟 = 25/25×440/2 = 220 MHz**
         * （sys.c 自己的注释也写 220 MHz，同文件下面读时序那段也是按 4.5ns/220MHz 算的）。
         * 两种说法在 5/5 这一档上刚好都算成 50.0 ns（11 周期 × 4.545 vs 10 周期 × 5.0），
         * 所以这个错误一直没被发现 —— 但它把"还有多少余量"的判断整个带偏了。
         *
         * 【真正的成本模型】一次 16 位写 = (ADDSET + DATAST + 1) / 220MHz。
         * 5/5 ⇒ 11 周期 ⇒ 50.0 ns/px，与板上实测的 50.0 ns/px **完全吻合**
         * （g_flush_cyc = 3071849 @400MHz ÷ 153600 px = 20.0 周期 = 50.0 ns）。
         * 关键推论：**只有 DATAST 受面板 tWRL ≥ 19ns 约束**（≥4 即可，即 22.7ns 低电平）；
         * ADDSET 只决定 CS/RS 建立时间，从来没被单独扫过。所以 5/5 的"总线地板"
         * 其实只是**当前寄存器设置**，不是硬件极限：
         *   ADDSET=1 + DATAST=4 ⇒ 6 周期 ⇒ 27.3 ns/px ⇒ 整屏 4.19 ms（省 45%）。
         *
         * 【2026-10-04 板上实测（tools/fmc_sweep.py --verify，每档都跑 A2 双自检）】
         *   档位   整屏 flush   ns/px    理论值   双自检
         *   5/5    7.68 ms     49.99    7.68     OK
         *   5/4    6.98 ms     45.45    6.98     OK
         *   4/4    6.28 ms     40.90    6.28     OK
         *   3/4    5.58 ms     36.35    5.59     OK
         *   2/4    4.89 ms     31.81    4.89     OK   ← 现取此档，比 5/5 省 2.79 ms
         *   1/4    4.19 ms     27.26    4.19     OK   （最快，但 ADDSET 只剩 4.5ns）
         * 实测与理论 (ADDSET+DATAST+1)/220MHz **六档全部吻合** ⇒ 反证了上一版注释里
         * 那句"FMC = 200MHz"是错的，fmc_ker_ck 确实是 PLL2_R = 220 MHz。
         *
         * 【为什么默认取 2/4 而不是最快的 1/4】DATAST 的下限是面板规格的硬约束
         * （tWRL ≥ 19ns ⇒ ≥4 周期），没有余地；ADDSET 是"够用就行"的建立时间，
         * 1 周期只有 4.5ns，余量太薄（长排线/温度变化就可能不够），而它只值 0.7 ms。
         * 2/4 把建立时间翻倍到 9.1ns，仍省 2.79 ms —— 留余量优先。
         * 要更激进：SWD 写 g_lcd_fmc_addset = 1 即可试，试完别忘了再跑一次 A2 双自检。
         *
         * 【怎么扫】这两个量搬成了全局量（见下方 g_lcd_fmc_*），
         * 用 SWD 写档位 + 写 1 到 main.c 的 g_fmc_timing_sweep 即可重配并重测，
         * **不必重新编译、不必复位**。换档后画面可能错乱（太快就会丢写），
         * 定档后必须复跑 A2 双自检（g_ramp_mismatch / g_gram_mismatch 都要为 0）。 */
        g_lcd_fmc_addset = 2;                   /* ADDSET：地址建立，2 周期 ≈ 9.1ns */
        g_lcd_fmc_datast = 4;                   /* DATAST：数据保持，4 周期 ≈ 22.7ns（面板 tWRL≥19ns） */
        lcd_fmc_write_timing_apply();
    }
    
    lcd_display_dir(0);                         /* 默认为竖屏 */
    LCD_BL(1);                                  /* 点亮背光 */
    lcd_clear(WHITE);
}

/**
 * @brief       清屏函数
 * @param       color: 要清屏的颜色
 * @retval      无
 */
void lcd_clear(uint16_t color)
{
    uint32_t index = 0;
    uint32_t totalpoint = lcddev.width;

    totalpoint *= lcddev.height;    /* 得到总点数 */
    lcd_set_cursor(0x00, 0x0000);   /* 设置光标位置 */
    lcd_write_ram_prepare();        /* 开始写入GRAM */

    for (index = 0; index < totalpoint; index++)
    {
        LCD->LCD_RAM = color;
    }
}

/**
 * @brief       在指定区域内填充单个颜色
 * @param       (sx,sy),(ex,ey):填充矩形对角坐标,区域大小为:(ex - sx + 1) * (ey - sy + 1)
 * @param       color: 要填充的颜色
 * @retval      无
 */
void lcd_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint32_t color)
{
    uint16_t i, j;
    uint16_t xlen = 0;
    
    xlen = ex - sx + 1;
    for (i = sy; i <= ey; i++)
    {
        lcd_set_cursor(sx, i);      /* 设置光标位置 */
        lcd_write_ram_prepare();    /* 开始写入GRAM */

        for (j = 0; j < xlen; j++)
        {
            LCD->LCD_RAM = color;   /* 显示颜色 */
        }
    }
}

/**
 * @brief       在指定区域内填充指定颜色块
 * @param       (sx,sy),(ex,ey):填充矩形对角坐标,区域大小为:(ex - sx + 1) * (ey - sy + 1)
 * @param       color: 要填充的颜色数组首地址
 * @retval      无
 */
void lcd_color_fill(uint16_t sx, uint16_t sy, uint16_t ex, uint16_t ey, uint16_t *color)
{
    uint16_t height, width;
    uint16_t i, j;    
    
    width = ex - sx + 1;                         /* 得到填充的宽度 */
    height = ey - sy + 1;                        /* 高度 */

    for (i = 0; i < height; i++)
    {
        lcd_set_cursor(sx, sy + i);              /* 设置光标位置 */
        lcd_write_ram_prepare();                 /* 开始写入GRAM */

        for (j = 0; j < width; j++)
        {
            LCD->LCD_RAM = color[i * width + j]; /* 写入数据 */
        }
    }
}

/**
 * @brief       画线
 * @param       x1,y1: 起点坐标
 * @param       x2,y2: 终点坐标
 * @param       color: 线的颜色
 * @retval      无
 */
void lcd_draw_line(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    uint16_t t;
    int xerr = 0, yerr = 0, delta_x, delta_y, distance;
    int incx, incy, row, col;
    delta_x = x2 - x1;                 /* 计算坐标增量 */
    delta_y = y2 - y1;
    row = x1;
    col = y1;

    if (delta_x > 0) incx = 1;         /* 设置单步方向 */
    else if (delta_x == 0) incx = 0;   /* 垂直线 */
    else
    {
        incx = -1;
        delta_x = -delta_x;
    }

    if (delta_y > 0) incy = 1;
    else if (delta_y == 0) incy = 0;   /* 水平线 */
    else
    {
        incy = -1;
        delta_y = -delta_y;
    }

    if ( delta_x > delta_y) distance = delta_x; /* 选取基本增量坐标轴 */
    else distance = delta_y;

    for (t = 0; t <= distance + 1; t++ )        /* 画线输出 */
    {
        lcd_draw_point(row, col, color);        /* 画点 */
        xerr += delta_x ;
        yerr += delta_y ;

        if (xerr > distance)
        {
            xerr -= distance;
            row += incx;
        }

        if (yerr > distance)
        {
            yerr -= distance;
            col += incy;
        }
    }
}

/**
 * @brief       画水平线
 * @param       x,y  : 起点坐标
 * @param       len  : 线长度
 * @param       color: 矩形的颜色
 * @retval      无
 */
void lcd_draw_hline(uint16_t x, uint16_t y, uint16_t len, uint16_t color)
{
    if ((len == 0) || (x > lcddev.width) || (y > lcddev.height)) return;

    lcd_fill(x, y, x + len - 1, y, color);
}

/**
 * @brief       画矩形
 * @param       x1,y1: 起点坐标
 * @param       x2,y2: 终点坐标
 * @param       color: 矩形的颜色
 * @retval      无
 */
void lcd_draw_rectangle(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, uint16_t color)
{
    lcd_draw_line(x1, y1, x2, y1, color);
    lcd_draw_line(x1, y1, x1, y2, color);
    lcd_draw_line(x1, y2, x2, y2, color);
    lcd_draw_line(x2, y1, x2, y2, color);
}

/**
 * @brief       画圆
 * @param       x0,y0: 圆中心坐标
 * @param       r    : 半径
 * @param       color: 圆的颜色
 * @retval      无
 */
void lcd_draw_circle(uint16_t x0, uint16_t y0, uint8_t r, uint16_t color)
{
    int a, b;
    int di;
    a = 0;
    b = r;
    di = 3 - (r << 1);       /* 判断下个点位置的标志 */

    while (a <= b)
    {
        lcd_draw_point(x0 + a, y0 - b, color);  /* 5 */
        lcd_draw_point(x0 + b, y0 - a, color);  /* 0 */
        lcd_draw_point(x0 + b, y0 + a, color);  /* 4 */
        lcd_draw_point(x0 + a, y0 + b, color);  /* 6 */
        lcd_draw_point(x0 - a, y0 + b, color);  /* 1 */
        lcd_draw_point(x0 - b, y0 + a, color);
        lcd_draw_point(x0 - a, y0 - b, color);  /* 2 */
        lcd_draw_point(x0 - b, y0 - a, color);  /* 7 */
        a++;

        /* 使用Bresenham算法画圆 */
        if (di < 0)
        {
            di += 4 * a + 6;
        }
        else
        {
            di += 10 + 4 * (a - b);
            b--;
        }
    }
}

/**
 * @brief       填充实心圆
 * @param       x,y  : 圆中心坐标
 * @param       r    : 半径
 * @param       color: 圆的颜色
 * @retval      无
 */
void lcd_fill_circle(uint16_t x, uint16_t y, uint16_t r, uint16_t color)
{
    uint32_t i;
    uint32_t imax = ((uint32_t)r * 707) / 1000 + 1;
    uint32_t sqmax = (uint32_t)r * (uint32_t)r + (uint32_t)r / 2;
    uint32_t xr = r;

    lcd_draw_hline(x - r, y, 2 * r, color);

    for (i = 1; i <= imax; i++)
    {
        if ((i * i + xr * xr) > sqmax)
        {
            /* draw lines from outside */
            if (xr > imax)
            {
                lcd_draw_hline (x - i + 1, y + xr, 2 * (i - 1), color);
                lcd_draw_hline (x - i + 1, y - xr, 2 * (i - 1), color);
            }

            xr--;
        }

        /* draw lines from inside (center) */
        lcd_draw_hline(x - xr, y + i, 2 * xr, color);
        lcd_draw_hline(x - xr, y - i, 2 * xr, color);
    }
}

/**
 * @brief       在指定位置显示一个字符
 * @param       x,y   : 坐标
 * @param       chr   : 要显示的字符:' '--->'~'
 * @param       size  : 字体大小 12/16/24/32
 * @param       mode  : 叠加方式(1); 非叠加方式(0);
 * @param       color : 字符的颜色;
 * @retval      无
 */
void lcd_show_char(uint16_t x, uint16_t y, char chr, uint8_t size, uint8_t mode, uint16_t color)
{
    uint8_t temp, t1, t;
    uint16_t y0 = y;
    uint8_t csize = 0;
    uint8_t *pfont = 0;

    csize = (size / 8 + ((size % 8) ? 1 : 0)) * (size / 2); /* 得到字体一个字符对应点阵集所占的字节数 */
    chr = chr - ' ';    /* 得到偏移后的值（ASCII字库是从空格开始取模，所以-' '就是对应字符的字库） */

    switch (size)
    {
        case 12:
            pfont = (uint8_t *)asc2_1206[chr];  /* 调用1206字体 */
            break;

        case 16:
            pfont = (uint8_t *)asc2_1608[chr];  /* 调用1608字体 */
            break;

        case 24:
            pfont = (uint8_t *)asc2_2412[chr];  /* 调用2412字体 */
            break;

        case 32:
            pfont = (uint8_t *)asc2_3216[chr];  /* 调用3216字体 */
            break;

        default:
            return ;
    }

    for (t = 0; t < csize; t++)
    {
        temp = pfont[t];                            /* 获取字符的点阵数据 */

        for (t1 = 0; t1 < 8; t1++)                  /* 一个字节8个点 */
        {
            if (temp & 0x80)                        /* 有效点,需要显示 */
            {
                lcd_draw_point(x, y, color);        /* 画点出来,要显示这个点 */
            }
            else if (mode == 0)                     /* 无效点并且选择非叠加方式 */
            {
                lcd_draw_point(x, y, g_back_color); /* 画背景色,相当于这个点不显示(注意背景色由全局变量控制) */
            }

            temp <<= 1;                             /* 移位, 以便获取下一个位的状态 */
            y++;

            if (y >= lcddev.height) return;         /* 超区域了 */

            if ((y - y0) == size)                   /* 显示完一列了? */
            {
                y = y0;                             /* y坐标复位 */
                x++;                                /* x坐标递增 */

                if (x >= lcddev.width) return;      /* x坐标超区域了 */

                break;
            }
        }
    }
}

/**
 * @brief       平方函数, m^n
 * @param       m: 底数
 * @param       n: 指数
 * @retval      m的n次方
 */
static uint32_t lcd_pow(uint8_t m, uint8_t n)
{
    uint32_t result = 1;

    while (n--)
    {
        result *= m;
    }
    
    return result;
}

/**
 * @brief       显示len个数字(高位为0则不显示)
 * @param       x,y   : 起始坐标
 * @param       num   : 数值(0 ~ 2^32)
 * @param       len   : 显示数字的位数
 * @param       size  : 选择字体 12/16/24/32
 * @param       color : 数字的颜色;
 * @retval      无
 */
void lcd_show_num(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint16_t color)
{
    uint8_t t, temp;
    uint8_t enshow = 0;

    for (t = 0; t < len; t++)                                               /* 按总显示位数循环 */
    {
        temp = (num / lcd_pow(10, len - t - 1)) % 10;                       /* 获取对应位的数字 */

        if (enshow == 0 && t < (len - 1))                                   /* 没有使能显示,且还有位要显示 */
        {
            if (temp == 0)
            {
                lcd_show_char(x + (size / 2) * t, y, ' ', size, 0, color);  /* 显示空格,占位 */
                continue;                                                   /* 继续下个一位 */
            }
            else
            {
                enshow = 1;                                                 /* 使能显示 */
            }
        }

        lcd_show_char(x + (size / 2) * t, y, temp + '0', size, 0, color);   /* 显示字符 */
    }
}

/**
 * @brief       扩展显示len个数字(高位是0也显示)
 * @param       x,y   : 起始坐标
 * @param       num   : 数值(0 ~ 2^32)
 * @param       len   : 显示数字的位数
 * @param       size  : 选择字体 12/16/24/32
 * @param       mode  : 显示模式
 *              [7]:0,不填充;1,填充0.
 *              [6:1]:保留
 *              [0]:0,非叠加显示;1,叠加显示.
 * @param       color : 数字的颜色;
 * @retval      无
 */
void lcd_show_xnum(uint16_t x, uint16_t y, uint32_t num, uint8_t len, uint8_t size, uint8_t mode, uint16_t color)
{
    uint8_t t, temp;
    uint8_t enshow = 0;

    for (t = 0; t < len; t++)                                                            /* 按总显示位数循环 */
    {
        temp = (num / lcd_pow(10, len - t - 1)) % 10;                                    /* 获取对应位的数字 */

        if (enshow == 0 && t < (len - 1))                                                /* 没有使能显示,且还有位要显示 */
        {
            if (temp == 0)
            {
                if (mode & 0x80)                                                         /* 高位需要填充0 */
                {
                    lcd_show_char(x + (size / 2) * t, y, '0', size, mode & 0x01, color); /* 用0占位 */
                }
                else
                {
                    lcd_show_char(x + (size / 2) * t, y, ' ', size, mode & 0x01, color); /* 用空格占位 */
                }

                continue;
            }
            else
            {
                enshow = 1;                                                              /* 使能显示 */
            }
        }

        lcd_show_char(x + (size / 2) * t, y, temp + '0', size, mode & 0x01, color);
    }
}

/**
 * @brief       显示字符串
 * @param       x,y         : 起始坐标
 * @param       width,height: 区域大小
 * @param       size        : 选择字体 12/16/24/32
 * @param       p           : 字符串首地址
 * @param       color       : 字符串的颜色
 * @retval      无
 */
void lcd_show_string(uint16_t x, uint16_t y, uint16_t width, uint16_t height, uint8_t size, char *p, uint16_t color)
{
    uint8_t x0 = x;
    
    width += x;
    height += y;

    while ((*p <= '~') && (*p >= ' '))   /* 判断是不是非法字符! */
    {
        if (x >= width)
        {
            x = x0;
            y += size;
        }

        if (y >= height)
        {
            break;                       /* 退出 */
        }

        lcd_show_char(x, y, *p, size, 0, color);
        x += size / 2;
        p++;
    }
}









/**
 ****************************************************************************************************					 
 * @file        sys.c
 * @version     V1.0
 * @brief       系统初始化代码(包括时钟配置/中断管理/GPIO设置等)            
 ****************************************************************************************************'
 *
 * V1.0
 * 将头文件包含路径改成相对路径,避免重复设置包含路径的麻烦
 *
 ****************************************************************************************************
 */

#include "./SYSTEM/sys/sys.h"

/* ---- 时钟初始化诊断量（2026-10-04 新增）------------------------------------
 * 配套说明见 sys.h。取值含义以 sys.h 的注释为准。
 * 初值取 0xFFFFFFFF / 0xFF：这样"从未被写过"与"写成了某个小整数"一眼可分，
 * 不会把"没跑到"误读成"跑到了但值小"。 */
volatile uint32_t g_sysclk_stage       = 0xFFFFFFFFu;
volatile uint32_t g_sysclk_err        = 0xFFu;
volatile uint32_t g_sysclk_vos_wait = 0u;

/* VOSRDY 死等的上限（毫秒）。
 * ★ 取值 1000 是**对齐 HAL 官方值**（`stm32h7xx_hal_pwr_ex.c:188`
 *   `#define PWR_FLAG_SETTING_DELAY (1000U)`）—— HAL 自己的
 *   HAL_PWREx_ConfigSupply() / ControlVoltageScaling() 等 ACTVOSRDY
 *   也用这个上限。原先取 200 是拍脑袋定的"宽松三个数量级"，实测证明不够。
 *
 * 实测（2026-10-04，两次上电对照）：
 *   烧录后冷启动：VOSRDY 在 200 ms 内**未**就绪（g_sysclk_vos_wait=201 撞上限），
 *                  但稍后读寄存器 VOSRDY=1 / ACTVOSRDY=1 ⇒ **它会自己就绪，只是慢**。
 *   probe-rs reset 后：立刻就是 1。
 *   两者差异的原因：SYSRESETREQ **不复位 PWR 域**，电压调节器状态被保留 ⇒ 无需重新爬升；
 *   真正断电则必须从复位档重新建立电压，故冷启动慢。
 *   ⚠ 本板 200~1000 ms 之间的具体就绪时刻未测得（探针连上去时已就绪），
 *     故取 HAL 官方值 1000，保守对齐。
 *
 * 为什么必须有限：无限等待会让整个固件静默死在这，外部只能看到"屏幕不亮"，
 * 与硬件故障完全无法区分（2026-10-04 为此白排查了很久）。
 * ⚠ 下游依赖：万一真的超时，系统仍跑在 HSI（VOS 停在复位档），主频约 64 MHz，
 *   一切都比标称慢，但**能跑、能被调试器读** —— 这正是超时诊断的目的。 */
#define SYS_VOSRDY_TIMEOUT_MS  1000u


/**
 * @brief       判断I_Cache是否打开
 * @param       无
 * @retval      返回值:0 关闭，1 打开
 */
uint8_t get_icahce_sta(void)
{
    uint8_t sta;
    sta = ((SCB->CCR)>>17) & 0X01;
    return sta;
}

/**
 * @brief       判断D_Cache是否打开
 * @param       无
 * @retval      返回值:0 关闭，1 打开
 */
uint8_t get_dcahce_sta(void)
{
    uint8_t sta;
    sta = ((SCB->CCR)>>16) & 0X01;
    return sta;
}

/**
 * @brief       设置中断向量表偏移地址
 * @param       baseaddr: 基址
 * @param       offset: 偏移量
 * @retval      无
 */
void sys_nvic_set_vector_table(uint32_t baseaddr, uint32_t offset)
{
    /* 设置NVIC的向量表偏移寄存器,VTOR低9位保留,即[8:0]保留 */
    SCB->VTOR = baseaddr | (offset & (uint32_t)0xFFFFFE00);
}

/**
 * @brief       执行: WFI指令(执行完该指令进入低功耗状态, 等待中断唤醒)
 * @param       无
 * @retval      无
 */
void sys_wfi_set(void)
{
    __ASM volatile("wfi");
}

/**
 * @brief       关闭所有中断(但是不包括fault和NMI中断)
 * @param       无
 * @retval      无
 */
void sys_intx_disable(void)
{
    __ASM volatile("cpsid i");
}

/**
 * @brief       开启所有中断
 * @param       无
 * @retval      无
 */
void sys_intx_enable(void)
{
    __ASM volatile("cpsie i");
}

/**
 * @brief       设置栈顶地址
 * @param       addr: 栈顶地址
 * @retval      无
 */
void sys_msr_msp(uint32_t addr)
{
    __set_MSP(addr);         /* 设置栈顶地址 */
}

/**
 * @brief       使能STM32H7的L1-Cache, 同时开启D cache的强制透写
 * @param       无
 * @retval      无
 */
void sys_cache_enable(void)
{
    SCB_EnableICache();      /* 使能I-Cache,函数在core_cm7.h里面定义 */
    SCB_EnableDCache();      /* 使能D-Cache,函数在core_cm7.h里面定义 */
    SCB->CACR |= 1 << 2;     /* 强制D-Cache透写,如不开启透写,实际使用中可能遇到各种问题 */
}

/**
 * @brief       时钟设置函数
 * @param       plln: PLL1 VCO的倍频系数(PLL倍频), 取值范围: 4~512.
 * @param       pllm: PLL1预分频系数(进PLL之前的分频), 取值范围: 1~63.
 * @param       pllp: PLL1的p分频系数(PLL之后的分频), 分频后作为系统时钟, 取值范围: 1~128.(除1外不能为奇数)
 * @param       pllq: PLL1的q分频系数(PLL之后的分频), 取值范围: 1~128.
 * @note
 *
 *              Fvco: VCO频率
 *              Fsys: 系统时钟频率, 也是PLL1的p分频输出时钟频率
 *              Fq:   PLL1的q分频输出时钟频率
 *              Fs:   PLL输入时钟频率, 可以是HSI, CSI, HSE等.
 *              Fvco = Fs * (plln / pllm);
 *              Fsys = Fvco / pllp = Fs * (plln / (pllm * pllp));
 *              Fq   = Fvco / pllq = Fs * (plln / (pllm * pllq));
 *
 *              外部晶振为25M的时候, 推荐值: plln = 160, pllm = 5, pllp = 2, pllq = 4.
 *              得到:Fvco = 25 * (160 / 5) = 800Mhz
 *                   Fsys = pll1_p_ck = 800 / 2 = 400Mhz
 *                   Fq   = pll1_q_ck = 800 / 4 = 200Mhz
 *
 *              H743默认需要配置的频率如下:
 *              CPU频率(rcc_c_ck) = sys_d1cpre_ck = 400Mhz
 *              rcc_aclk = rcc_hclk3 = 200Mhz
 *              AHB1/2/3/4(rcc_hclk1/2/3/4) = 200Mhz
 *              APB1/2/3/4(rcc_pclk1/2/3/4) = 100Mhz
 *              pll2_p_ck = (25 / 25) * 440 / 2) = 220Mhz
 *              pll2_r_ck = FMC时钟频率 = ((25 / 25) * 440 / 2) = 220Mhz
 *
 * @retval      错误代码: 0, 成功; 1, 错误;
 */
uint8_t sys_stm32_clock_init(uint32_t plln, uint32_t pllm, uint32_t pllp, uint32_t pllq)
{
    HAL_StatusTypeDef ret = HAL_OK;
    RCC_ClkInitTypeDef rcc_clk_init_handle;
    RCC_OscInitTypeDef rcc_osc_init_handle;
    RCC_PeriphCLKInitTypeDef rcc_periph_clk_init;

    /* 每次进来先清一次，保证重入时不会读到上一轮的旧阶段值 */
    g_sysclk_stage       = 1u;
    g_sysclk_err        = 0u;
    g_sysclk_vos_wait = 0u;

    __HAL_RCC_SYSCFG_CLK_ENABLE();                                  /* 使能SYSCFG外设时钟 */
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);  /* 电压调节选择Scale1, 1.20V内核电压 */
    g_sysclk_stage = 2u;

    /* 等待电压稳定 —— 原厂这里是 `while(...){}` **无超时死等**（见下方说明）。
     * 改为有上限的等待：超时就带着"未就绪"继续往下走，让后续初始化照常执行，
     * 由 g_sysclk_stage/g_sysclk_err 把这个事实暴露到 RAM，而不是把板子挂死。 */
    {
        uint32_t t0 = HAL_GetTick();

        while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY))
        {
            g_sysclk_vos_wait = HAL_GetTick() - t0;

            if (g_sysclk_vos_wait > (uint32_t)SYS_VOSRDY_TIMEOUT_MS)
            {
                /* VOSRDY 未就绪。两种常见成因（都不是"代码写错"）：
                 *   ① 供电模式没配：HAL 要求先调 HAL_PWREx_ConfigSupply() 再设档位，
                 *      本工程从未调用过它（见 stm32h7xx_hal_pwr_ex.c 顶部流程说明）；
                 *   ② VOSRDY 本来就不是靠等出来的，硬件没给这个条件。
                 * 此时 VOS 停在复位档（Scale3），主频只有 HSI 的 64 MHz，
                 * 后面所有 FMC/LCD 时序都会随之变慢 —— 但功能上仍可继续。 */
                g_sysclk_err = 2u;

                /* 不直接 return：留在这里会让 LCD/触摸/YMGUI 一律初始化不了，
                 * 变成一个"全黑板"，反而更难诊断。继续往下走，让 LCD 等
                 * 各自把自己的失败记下来（那些路径本来就有错误码）。 */
                break;
            }
        }
        g_sysclk_stage = 3u;
    }

    /* 使能HSE，并选择HSE作为PLL时钟源，配置PLL1，开启HSI48时钟 */
    rcc_osc_init_handle.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI48;
    rcc_osc_init_handle.HSEState = RCC_HSE_ON;
    rcc_osc_init_handle.HSIState = RCC_HSI_OFF;
    rcc_osc_init_handle.CSIState = RCC_CSI_OFF;
    rcc_osc_init_handle.HSI48State = RCC_HSI48_ON;
    rcc_osc_init_handle.PLL.PLLState = RCC_PLL_ON;
    rcc_osc_init_handle.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    rcc_osc_init_handle.PLL.PLLN = plln;
    rcc_osc_init_handle.PLL.PLLM = pllm;
    rcc_osc_init_handle.PLL.PLLP = pllp;
    rcc_osc_init_handle.PLL.PLLQ = pllq;
    rcc_osc_init_handle.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
    rcc_osc_init_handle.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
    rcc_osc_init_handle.PLL.PLLFRACN = 0;
    ret = HAL_RCC_OscConfig(&rcc_osc_init_handle);

    if (ret != HAL_OK)
    {
        g_sysclk_err = 1u;
        return 1;
    }
    g_sysclk_stage = 4u;

    /*
     *  选择PLL的输出作为系统时钟
     *  配置RCC_CLOCKTYPE_SYSCLK系统时钟,400M
     *  配置RCC_CLOCKTYPE_HCLK 时钟,200Mhz,对应AHB1，AHB2，AHB3和AHB4总线
     *  配置RCC_CLOCKTYPE_PCLK1时钟,100Mhz,对应APB1总线
     *  配置RCC_CLOCKTYPE_PCLK2时钟,100Mhz,对应APB2总线
     *  配置RCC_CLOCKTYPE_D1PCLK1时钟,100Mhz,对应APB3总线
     *  配置RCC_CLOCKTYPE_D3PCLK1时钟,100Mhz,对应APB4总线
     */
    rcc_clk_init_handle.ClockType = (RCC_CLOCKTYPE_SYSCLK \
                                    | RCC_CLOCKTYPE_HCLK \
                                    | RCC_CLOCKTYPE_PCLK1 \
                                    | RCC_CLOCKTYPE_PCLK2 \
                                    | RCC_CLOCKTYPE_D1PCLK1 \
                                    | RCC_CLOCKTYPE_D3PCLK1);

    rcc_clk_init_handle.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    rcc_clk_init_handle.SYSCLKDivider = RCC_SYSCLK_DIV1;
    rcc_clk_init_handle.AHBCLKDivider = RCC_HCLK_DIV2;
    rcc_clk_init_handle.APB1CLKDivider = RCC_APB1_DIV2; 
    rcc_clk_init_handle.APB2CLKDivider = RCC_APB2_DIV2; 
    rcc_clk_init_handle.APB3CLKDivider = RCC_APB3_DIV2;  
    rcc_clk_init_handle.APB4CLKDivider = RCC_APB4_DIV2; 
    ret = HAL_RCC_ClockConfig(&rcc_clk_init_handle, FLASH_LATENCY_2);

    if (ret != HAL_OK)
    {
        g_sysclk_err = 1u;
        return 1;
    }
    g_sysclk_stage = 5u;

    /*
     *  配置PLL2的R分频输出, 为220Mhz
     *  配置FMC时钟源是pll2_r_ck时钟
     */
    rcc_periph_clk_init.PeriphClockSelection = RCC_PERIPHCLK_FMC;
    rcc_periph_clk_init.PLL2.PLL2M = 25;
    rcc_periph_clk_init.PLL2.PLL2N = 440;
    rcc_periph_clk_init.PLL2.PLL2P = 2;
    rcc_periph_clk_init.PLL2.PLL2R = 2;
    rcc_periph_clk_init.PLL2.PLL2RGE = RCC_PLL2VCIRANGE_0;
    rcc_periph_clk_init.PLL2.PLL2VCOSEL = RCC_PLL2VCOWIDE;
    rcc_periph_clk_init.PLL2.PLL2FRACN = 0;
    rcc_periph_clk_init.FmcClockSelection = RCC_FMCCLKSOURCE_PLL2;
    ret = HAL_RCCEx_PeriphCLKConfig(&rcc_periph_clk_init);

    if (ret != HAL_OK)
    {
        g_sysclk_err = 1u;
        return 1;
    }
    g_sysclk_stage = 6u;      /* 时钟初始化全程完成（注意 g_sysclk_err 可能仍为 2：VOSRDY 超时但后续走完） */

    HAL_PWREx_EnableUSBVoltageDetector();   /* 使能USB电压电平检测器 */
    __HAL_RCC_CSI_ENABLE() ;                /* 使能CSI时钟, 为I/O补偿单元提供时钟 */
    __HAL_RCC_SYSCFG_CLK_ENABLE() ;         /* 使能SYSCFG时钟 */
    HAL_EnableCompensationCell();           /* 使能I/O补偿单元 */
    return 0;
}

#ifdef  USE_FULL_ASSERT

/**
 * @brief       当编译提示出错的时候此函数用来报告错误的文件和所在行
 * @param       file：指向源文件
 * @param       line：指向在文件中的行数
 * @retval      无
 */
void assert_failed(uint8_t *file, uint32_t line)
{ 
    while (1)
    {
    }
}

#endif








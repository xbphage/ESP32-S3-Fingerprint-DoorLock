#ifndef _fingerID_HPP
#define _fingerID_HPP
#pragma once

//#define TEST
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "soc\gpio_num.h"
#include "esp_timer.h"
#include <portmacro.h>
#include "esp_log.h"

#define PACKHEAD ((uint16_t)0xEF01) //通信包头
#define COMMANDSIGN ((uint8_t)0x01) //命令包标识
#define CharBuffer1 0x01
#define CharBuffer2 0x02

#define UART_NUM_ID UART_NUM_1
#define UART_NUM_ID_TX GPIO_NUM_17
#define UART_NUM_ID_RX GPIO_NUM_18
const int RX_BUF_SIZE = 1024;  //串口接收缓冲区大小

// ---- 低功耗相关硬件定义 ----------------------------------------------

// 指纹模组 VCC 控制脚。高电平供电、低电平断电，经外部开关电路驱动
// （见规格书第 5 章"低功耗参考设计"：CTRL -> Q1(9013) -> Tr1(SI2301) -> 模组 VCC）。
#define ID_VCC_GPIO        GPIO_NUM_7

// 模组 TOUCH_OUT（PIN 2）接到 MCU 的哪只脚。
// 必须满足两个条件：① 是 RTC IO（ESP32-S3 上即 GPIO0~GPIO21），否则不能做
// Deep-sleep 的 EXT0 唤醒源；② 不与已占用的 7(VCC) / 8(PWM) / 17(TX) / 18(RX) 冲突。
#define ID_TOUCH_OUT_GPIO  GPIO_NUM_6

// 模组上电后等待其启动的时间（规格书标称启动时间 < 0.1s，这里留足余量）
#define ID_POWER_ON_DELAY_MS  300

// 【已废弃】原 press_FR() 的超时默认值。press_FR 被 Auto_Verify() 取代后无人使用，
// 识别超时现在由 ID_VERIFY_TIMEOUT_MS 负责。保留仅为不删历史定义。
#define ID_IDENTIFY_TIMEOUT_MS  10000

// ---- 自动注册 / 自动验证（手册 3.3.2 模块指令集）---------------------

#define ID_ENROLL_TIMES        5       // 自动注册的录入次数，手册规定 2~10 次
#define ID_VERIFY_LEVEL        3       // 自动验证的分数等级(0~9)，越大越严格
#define ID_TEMPLATE_MAX        100     // ZW111 指纹库容量，规格书标称 100 枚
#define ID_VERIFY_ALL_ID       0xFFFF  // PS_AutoIdentify 的 ID 号填此值表示 1:N 搜索
#define ID_ENROLL_TIMEOUT_MS   90000   // 自动注册整体超时（要按 5 次手指，给足时间）
#define ID_VERIFY_TIMEOUT_MS   15000   // 自动验证超时

// 呼吸灯颜色码（手册表 3-101）：bit0=蓝 bit1=绿 bit2=红
#define ID_LED_BLUE            0x01
#define ID_LED_GREEN           0x02
#define ID_LED_RED             0x04
#define ID_LED_OFF             0x00

// 呼吸灯功能码（手册 3.5.7）：01呼吸 02闪烁 03常开 04常闭 05渐开 06渐灭 07跑马
#define ID_LED_FUNC_BREATH     0x01
#define ID_LED_FUNC_BLINK      0x02
#define ID_LED_FUNC_ON         0x03
#define ID_LED_FUNC_OFF        0x04

// 自动注册 PS_AutoEnroll 的参数位（手册表 3-50 辅助说明）
#define ID_ENROLL_P_LEDOFF     (1u << 0)  // 1=采图成功后灭背光灯
#define ID_ENROLL_P_NOCOVER    (1u << 3)  // 1=允许覆盖已占用的 ID
#define ID_ENROLL_P_NODUP      (1u << 4)  // 1=不允许指纹重复注册
#define ID_ENROLL_P_NOLIFT     (1u << 5)  // 1=录入过程中不要求手指离开
// bit2 必须为 0（要求返回关键步骤），否则收不到进度包、无法判断注册是否完成

// 自动验证 PS_AutoIdentify 的参数位（手册表 3-53 辅助说明）
#define ID_VERIFY_P_LEDOFF     (1u << 0)  // 1=采图成功后灭背光灯

// 模块应答包的解析结果。
// 应答包结构：包头(2) 地址(4) 包标识(1) 包长度(2) | 确认码(1) 参数(n) 校验和(2)
// 其中"包长度"计的是从确认码到校验和的字节数，所以参数区长度 = 包长度 - 3。
struct AckPacket
{
    uint8_t ack;          // 确认码
    uint8_t param[40];    // 参数区（不含确认码和校验和）
    uint8_t param_len;    // 参数区实际长度
};

extern esp_timer_handle_t sleep_timer;

// 模组供电控制（不依赖 IDENTIFIER 实例，唤醒后需在构造对象之前调用）
void ID_PowerOn(void);
void ID_PowerOff(void);


class IDENTIFIER
{
    private:

    uint32_t IDaddr = 0XFFFFFFFF;
    uint32_t IDpwd = 0x00000000;//口令验证

    typedef struct  
    {
        uint16_t pageID;//指纹ID
        uint16_t mathscore;//匹配得分
    }SearchResult;

    typedef struct
    {
        uint16_t PS_max;//指纹最大容量
        uint8_t  PS_level;//安全等级
        uint32_t PS_addr;
        uint8_t  PS_size;//通讯数据包大小
        uint8_t  PS_N;//波特率基数N
    }SysPara;

    uart_config_t uart2id_config ;


    void init_uart2id(void);
    void IDUARTwrite_Bytes(uint8_t data);
    void IDUARTwrite_Bytes(uint16_t data);
    void IDUARTwrite_Bytes(uint32_t data);
    void SendHead(void);//发送包头
    void SendAddr(void);//发送地址
    void SendFlag(uint8_t flag);//发送包标志
    void SendLength(uint16_t length);//发送包长度
    void Sendcmd(uint8_t cmd);//发送指令
    void SendCheck(uint16_t check);//发送校验和
    uint8_t *JudgeStr();//判断中断接收的数组有没有应答包
    bool ReadAckPacket(AckPacket *pkt, uint32_t timeout_ms);//按长度精确读一个完整应答包
    void SendAutoEnrollCmd(uint16_t id, uint8_t times, uint16_t params);//组装自动注册指令包
    //呼吸灯控制底层包（3CH）。PS_LedCtrl 是它把颜色写死成全灭的简化版
    uint8_t PS_LedCtrlEx(uint8_t func, uint8_t startColor, uint8_t endColor, uint8_t loop);
    // ---- 以下 7 个原语已被 Auto_Enroll(31H)/Auto_Verify(32H) 取代，实现整段注释，需要时恢复 ----
    // uint8_t PS_GetImage(void); //录入图像
    // uint8_t PS_GenChar(uint8_t BufferID);//生成特征
    // uint8_t PS_Match(void);//精确比对两枚指纹特征
    // uint8_t PS_Search(uint8_t BufferID,uint16_t StartPage,uint16_t PageNum,SearchResult *p);//搜索指纹
    // uint8_t PS_RegModel(void);//合并特征（生成模板）
    // uint8_t PS_StoreChar(uint8_t BufferID,uint16_t PageID);//储存模板
    // uint8_t PS_HighSpeedSearch(uint8_t BufferID,uint16_t StartPage,uint16_t PageNum,SearchResult *p);//高速搜索

    uint8_t PS_DeletChar(uint16_t PageID,uint16_t N);//删除模板
    uint8_t PS_Empty(void);//清空指纹库
    uint8_t PS_WriteReg(uint8_t RegNum,uint8_t DATA);//写系统寄存器
    uint8_t PS_ReadSysPara(SysPara *p); //读系统基本参数
    uint8_t PS_SetAddr(uint32_t addr);  //设置模块地址
    uint8_t PS_WriteNotepad(uint8_t NotePageNum,uint8_t *content);//写记事本
    uint8_t PS_ReadNotepad(uint8_t NotePageNum,uint8_t *note);//读记事
    uint8_t PS_ValidTempleteNum(uint16_t *ValidN);//读有效模板个数
    uint32_t PS_GetRandomCode();//让模块发送一个随机数
    
    const char *EnsureMessage(uint8_t ensure);//确认码错误信息解析
    void ShowErrMessage(uint8_t ensure);


    public:

    IDENTIFIER();
    ~IDENTIFIER();

    // ---- 以下 2 个已被 Auto_Enroll / Auto_Verify 取代，实现整段注释，需要时恢复 ----
    // void Add_FR(void);          //手动注册（按两次手指）  -> 改 Auto_Enroll()
    // bool press_FR(uint32_t timeout_ms = ID_IDENTIFY_TIMEOUT_MS);  //手动验证循环 -> 改 Auto_Verify()
    void ID_SetSleepTime(uint8_t val);//设置休眠时间
    void Del_FR(void);
    void Del_FR_Lib(void);
    //按 ID 删除单枚模板（0CH）。成功 true，失败已自行打印原因。
    //Del_FR() 是早期写死"删 ID=1"的演示版，实际要删哪枚请用这个。
    bool Del_FR_ID(uint16_t id);
    bool AS608_Check(void);//连接检查
    bool Is_Touch(void);
    uint8_t PS_HandShake(uint32_t *PS_Addr); //与AS608模块握手
    uint8_t PS_Sleep(void);

    uint8_t PS_LedCtrl(uint8_t func); //呼吸灯控制 3CH：01呼吸/02闪烁/03常开/04常闭/05渐开/06渐灭
    uint8_t PS_LedOff(void);          //休眠前关灯（3CH + 04H 常闭灯）
    uint8_t PS_LedAuto(void);         //恢复出厂自动呼吸灯（60H + FFH）

    uint8_t PS_Cancel(void);          //取消自动注册/自动验证（30H）
    uint8_t PS_ReadIndexTable(uint8_t page, uint8_t *index32); //读模板索引表（1FH），每 bit 一枚模板
    uint16_t FindFreeID(uint16_t start = 0, uint16_t end = ID_TEMPLATE_MAX - 1); //找下一个空闲模板 ID

    //自动注册模板（31H）：自动挑一个空闲 ID，连续录入 times 次后合成模板存库
    //返回: 成功 true，outID 回填实际使用的 ID
    bool Auto_Enroll(uint16_t *outID, uint8_t times = ID_ENROLL_TIMES,
                     uint32_t timeout_ms = ID_ENROLL_TIMEOUT_MS);

    //自动验证指纹（32H）：id 填 ID_VERIFY_ALL_ID 则 1:N 搜索，否则与指定 ID 做 1:1 匹配
    //返回: 验证通过 true，outID/outScore 回填匹配到的模板号与得分
    bool Auto_Verify(uint16_t *outID, uint16_t *outScore = nullptr,
                     uint16_t id = ID_VERIFY_ALL_ID, uint8_t level = ID_VERIFY_LEVEL,
                     uint32_t timeout_ms = ID_VERIFY_TIMEOUT_MS);

    void EnterDeepSleep(void);        //关灯 -> 模组休眠 -> 断 VCC -> 配置 TOUCH_OUT 唤醒 -> Deep-sleep（不返回）
};


// 重置"t 秒内收不到模组数据就休眠"的倒计时。t == 0 表示立刻休眠。
// 倒计时到点后由定时器回调直接调 EnterDeepSleep()，函数不返回、芯片复位重跑。
// 每次收到模组数据（JudgeStr / ReadAckPacket）都会调它把倒计时往后推。
void ZW_Sleep(uint8_t t, IDENTIFIER &zw);


#endif
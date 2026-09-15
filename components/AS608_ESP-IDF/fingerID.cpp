/*******************************************************************************
****文件路径         : .\main\fingerID.cpp
****作者名称         : Scaxlibur
****文件版本         : V1.0.0
****创建日期         : 2024-10-21 20:25:26
****简要说明         : 指纹识别模块
********************************************************************************/

#include "fingerID.hpp"
#include <portmacro.h>
#include "esp_log.h"
static const char *SLEEP_TAG = "AS608_SLEEP";
static const char *ENROLL_TAG = "AS608_AUTO";
static const char *LINK_TAG = "AS608_LINK";
esp_timer_handle_t sleep_timer = nullptr;
// 正在执行休眠指令期间置 true。休眠指令自身也会收到模块应答，
// 若不加这个闸门，应答就会把倒计时重置，模块变成每 60 秒醒一次、睡一次，永远睡不下去。
static bool s_sending_sleep = false;

// 收到模块数据后调用：把"60 秒收不到数据就休眠"的倒计时往后推。
static void TouchSleepTimer(IDENTIFIER &id)
{
    if (!s_sending_sleep){
        ZW_Sleep(60, id);
    }
}

// ---- 模组供电控制（IO7 -> 外部开关电路 -> 模组 VCC）------------------
// 参考规格书第 5 章 "低功耗参考设计"：CTRL 高电平 -> Q1 导通 -> Tr1 栅极被拉低
// -> SI2301 导通 -> VCC 上电；CTRL 低电平则反之，模组断电。
// 这两个函数刻意做成自由函数而不是成员函数：唤醒后必须先给模组上电、
// 等它启动完，才能去构造 IDENTIFIER 做握手，那时对象还不存在。
static bool s_vcc_gpio_ready = false;

void ID_PowerOn(void)
{
    if (!s_vcc_gpio_ready){
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << ID_VCC_GPIO;
        cfg.mode         = GPIO_MODE_OUTPUT;
        cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
        cfg.intr_type    = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&cfg));
        s_vcc_gpio_ready = true;
    }
    gpio_set_level(ID_VCC_GPIO, 1);
    ESP_LOGI(SLEEP_TAG, "指纹模组 VCC 已上电(IO%d)", (int)ID_VCC_GPIO);
}

void ID_PowerOff(void)
{
    gpio_set_level(ID_VCC_GPIO, 0);
    ESP_LOGI(SLEEP_TAG, "指纹模组 VCC 已断电(IO%d)", (int)ID_VCC_GPIO);
}

IDENTIFIER::IDENTIFIER(void){
    init_uart2id();
    printf("指纹识别器对象已创建\n");
    vTaskDelay(200 / portTICK_PERIOD_MS);
    AS608_Check();
}

// UART 驱动由 init_uart2id() 注册，且用 uart_is_driver_installed() 去重，
// 属于进程级共享资源，不随单个对象析构释放，避免多实例之间相互破坏。
IDENTIFIER::~IDENTIFIER(void){
}

void IDENTIFIER::init_uart2id(void){
    if(uart_is_driver_installed(UART_NUM_ID) == false)//注册驱动前检查，避免重复注册报错
    {
        uart2id_config.baud_rate = 57600;   // AS608 出厂默认波特率
        uart2id_config.data_bits = UART_DATA_8_BITS;
        uart2id_config.parity = UART_PARITY_DISABLE;
        uart2id_config.stop_bits = UART_STOP_BITS_1;
        uart2id_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
        uart2id_config.source_clk = UART_SCLK_APB;
        ESP_ERROR_CHECK(uart_param_config(UART_NUM_ID, &uart2id_config));
        ESP_ERROR_CHECK(uart_set_pin(UART_NUM_ID, UART_NUM_ID_TX, UART_NUM_ID_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
        ESP_ERROR_CHECK(uart_driver_install(UART_NUM_ID, RX_BUF_SIZE * 2, 0, 0, NULL, 0));
    };
}

void IDENTIFIER::IDUARTwrite_Bytes(uint8_t data)
{
    uart_write_bytes(UART_NUM_ID, &data, 1);
}


void IDENTIFIER::IDUARTwrite_Bytes(uint16_t data)
{
    uint8_t data1 = data >> 8;
    uart_write_bytes(UART_NUM_ID, &data1, 1);
    uart_write_bytes(UART_NUM_ID, &data, 1);
}

void IDENTIFIER::IDUARTwrite_Bytes(uint32_t data)
{
    uint8_t data1 = data >> 24;
    uint8_t data2 = data >> 16;
    uint8_t data3 = data >> 8;
    uart_write_bytes(UART_NUM_ID, &data1, 1);
    uart_write_bytes(UART_NUM_ID, &data2, 1);
    uart_write_bytes(UART_NUM_ID, &data3, 1);
    uart_write_bytes(UART_NUM_ID, &data, 1);
}

void IDENTIFIER::SendHead(void)
{
    ESP_ERROR_CHECK(uart_flush(UART_NUM_ID));
    IDUARTwrite_Bytes(PACKHEAD);
}

void IDENTIFIER::SendAddr(void)
{

    IDUARTwrite_Bytes(IDaddr);
}

void IDENTIFIER::SendFlag(uint8_t flag)
{
    IDUARTwrite_Bytes(flag);
}

void IDENTIFIER::SendLength(uint16_t length)
{
  IDUARTwrite_Bytes(length);
}

void IDENTIFIER::Sendcmd(uint8_t cmd)
{
  IDUARTwrite_Bytes(cmd);
}

void IDENTIFIER::SendCheck(uint16_t check)
{
  IDUARTwrite_Bytes(check);
}

/*****************************************
函数名：bool AS608_Check(void)
参数：无
功能描述：模块是否连接检测 
返回值：模块连接了返回true 否则返回false
*****************************************/
// 连接检测的三段时序
#define AS608_ACK_WAIT_MS     80     // 发完指令先静等这么久，再看线路上有没有字节
#define AS608_ACK_TIMEOUT_MS  300    // 收一个完整应答包的超时
#define AS608_CHECK_RETRY     3      // 握手重试次数

bool IDENTIFIER::AS608_Check(void)
{
    // 用 13H VfyPwd（口令验证）当探针：它是模组上唯一一条不用先采图、发过去就能
    // 立刻回答的指令，最适合用来回答"模组在不在、这条串口通不通"。
    //
    // 判据不能是"缓冲区里有没有字节"。RX 悬空时线路上会拾到工频杂波，那个判据会把
    // 压根没接的模组判成连上了；反过来，波特率写错（有过把 57600 写成 75600 的先例）
    // 模组是一个字都不回的。这两种失败要查的东西完全不一样，所以这里交给
    // ReadAckPacket() 做完整的包头+长度校验，再按确认码下结论。
    uint8_t ack        = 0xff;
    bool    got_packet = false;
    bool    saw_bytes  = false;   // 这一次尝试里，线路上到底有没有出现过数据

    for (int attempt = 1; attempt <= AS608_CHECK_RETRY; attempt++)
    {
        uart_flush(UART_NUM_ID);    // 清掉上一轮残留，免得把旧应答当成这一轮的
        SendHead();
        SendAddr();
        SendFlag(COMMANDSIGN);
        SendLength((uint16_t)0x07);
        Sendcmd((uint8_t)0x13);
        IDUARTwrite_Bytes(IDpwd);
        uint16_t sum = 0x07 + 0x13 + IDpwd;     // = 0x1A（口令是 4 个 0 字节）
        SendCheck(sum);

        vTaskDelay(pdMS_TO_TICKS(AS608_ACK_WAIT_MS));
        size_t pending = 0;
        if (uart_get_buffered_data_len(UART_NUM_ID, &pending) == ESP_OK && pending > 0)
            saw_bytes = true;

        AckPacket pkt;
        got_packet = ReadAckPacket(&pkt, AS608_ACK_TIMEOUT_MS);
        ack = got_packet ? pkt.ack : 0xff;

        if (got_packet && ack == 0x00)
        {
            m_online = true;
            ESP_LOGI(LINK_TAG, "握手成功：13H 应答确认码 0x00（第 %d/%d 次尝试）",
                     attempt, AS608_CHECK_RETRY);
            return true;
        }

        if (attempt < AS608_CHECK_RETRY)
        {
            ESP_LOGW(LINK_TAG, "握手第 %d/%d 次失败（%s），重试",
                     attempt, AS608_CHECK_RETRY,
                     got_packet ? "确认码非 0x00" : "没收到合法应答包");
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }

    m_online = false;

    // 失败原因分开报。三类问题的排查方向完全不同，只丢一句"握手失败"等于没报。
    if (got_packet)
        ESP_LOGE(LINK_TAG, "握手失败：模组有应答，但确认码是 0x%02X（0x00 才算成功）",
                 (unsigned)ack);
    else if (saw_bytes)
        ESP_LOGE(LINK_TAG, "握手失败：收到了数据，但没有一帧是合法应答包。"
                 "优先查 TX/RX 是否接反、电平是否匹配，其次才是波特率");
    else
        ESP_LOGE(LINK_TAG, "握手失败：模组无任何应答。查模组是否已上电"
                 "（ID_VCC_GPIO=%d 应为高电平）、TX/RX 是否交叉、波特率是否 57600",
                 (int)ID_VCC_GPIO);
    return false;
}

uint8_t* IDENTIFIER::JudgeStr()
{
    static uint8_t receive[64];   // 静态存储,生命周期到程序结束
    uint8_t str[8];

    str[0] = 0xEF;
    str[1] = 0x01;
    str[2] = IDaddr >> 24;
    str[3] = IDaddr >> 16;
    str[4] = IDaddr >> 8;
    str[5] = IDaddr;
    str[6] = 0x07;
    str[7] = '\0';

    size_t uartSize = 0;
    vTaskDelay(500 / portTICK_PERIOD_MS);
    ESP_ERROR_CHECK(uart_get_buffered_data_len(UART_NUM_ID, &uartSize));

    if (uartSize >= sizeof(receive))      // 防越界
        uartSize = sizeof(receive) - 1;

    if (uartSize > 0){
        uart_read_bytes(UART_NUM_ID, receive, uartSize, 500 / portTICK_PERIOD_MS);
        TouchSleepTimer(*this);           // 收到模块数据，推迟休眠
    }
    receive[uartSize] = '\0';             // 保证 strstr 安全

    return (uint8_t *)strstr((const char *)receive, (const char *)str);
}

// ===========================================================================
// [已注释] PS_GetImage —— 采图原语。原供手动注册(press 两次)流程使用，现由 Auto_Enroll(31H) 一体化完成
// 需要恢复时，把本段每一行行首的 "// " 删掉即可。
// ===========================================================================
// //录入图像 PS_GetImage
// //功能:探测手指，探测到后录入指纹图像存于ImageBuffer。
// //模块返回确认字
// uint8_t IDENTIFIER::PS_GetImage(void)
// {
//     uint16_t sum;
//     uint8_t  ensure;
//     uint8_t  *data = NULL;
//     SendHead();
//     SendAddr();
//     SendFlag(COMMANDSIGN);
//     SendLength(0x03);
//     Sendcmd(0x01);
//     sum =  0x01 + 0x03 + 0x01;
//     SendCheck(sum);
//     data = JudgeStr();
//     if(data)
//         ensure = data[9];
//     else
//         ensure = 0xff;
//     return ensure;
// }

// ===========================================================================
// [已注释] PS_GenChar —— 生成特征原语。同上，由 Auto_Enroll(31H) 取代
// 需要恢复时，把本段每一行行首的 "// " 删掉即可。
// ===========================================================================
// //生成特征 PS_GenChar
// //功能:将ImageBuffer中的原始图像生成指纹特征文件存于CharBuffer1或CharBuffer2
// //参数:BufferID --> charBuffer1:0x01	charBuffer1:0x02
// //模块返回确认字
// uint8_t IDENTIFIER::PS_GenChar(uint8_t BufferID)
// {
//     uint16_t sum;
//     uint8_t  ensure;
//     uint8_t  *data = NULL;
//     SendHead();
//     SendAddr();
//     SendFlag(COMMANDSIGN);
//     SendLength(0x04);
//     Sendcmd(0x02);
//     IDUARTwrite_Bytes(BufferID);
//     sum = 0x01 + 0x04 + 0x02 + BufferID;
//     SendCheck(sum);
//     data = JudgeStr();
//     if(data)
//         ensure = data[9];
//     else
//         ensure = 0xff;
//     return ensure;
// }

// ===========================================================================
// [已注释] PS_Match —— 1:1 精确比对原语。已由 Auto_Verify(32H) 的 id 参数指定模板号实现
// 需要恢复时，把本段每一行行首的 "// " 删掉即可。
// ===========================================================================
// //精确比对两枚指纹特征 PS_Match
// //功能:精确比对CharBuffer1 与CharBuffer2 中的特征文件
// //模块返回确认字
// uint8_t IDENTIFIER::PS_Match(void)
// {
//     uint16_t sum;
//     uint8_t  ensure;
//     uint8_t  *data = NULL;
//     SendHead();
//     SendAddr();
//     SendFlag(COMMANDSIGN);
//     SendLength(0x03);
//     Sendcmd(0x03);
//     sum = 0x01 + 0x03 + 0x03;
//     SendCheck(sum);
//     data = JudgeStr();
//     if(data)
//         ensure = data[9];
//     else
//         ensure = 0xff;
//     return ensure;
// }

// ===========================================================================
// [已注释] PS_Search —— 1:N 搜索原语。已由 Auto_Verify(32H) 取代（本函数在改动前就已无调用者）
// 需要恢复时，把本段每一行行首的 "// " 删掉即可。
// ===========================================================================
// //搜索指纹 PS_Search
// //功能:以CharBuffer1或CharBuffer2中的特征文件搜索整个或部分指纹库.若搜索到，则返回页码。
// //参数:  BufferID @ref CharBuffer1	CharBuffer2
// //说明:  模块返回确认字，页码（相配指纹模板）
// uint8_t IDENTIFIER::PS_Search(uint8_t BufferID, uint16_t StartPage, uint16_t PageNum, SearchResult *p)
// {
//     uint16_t sum;
//     uint8_t  ensure;
//     uint8_t  *data = NULL;
//     SendHead();
//     SendAddr();
//     SendFlag(COMMANDSIGN);//命令包标识
//     SendLength(0x08);
//     Sendcmd(0x04);
//     IDUARTwrite_Bytes(BufferID);
//     IDUARTwrite_Bytes(StartPage);
//     IDUARTwrite_Bytes(PageNum);
//     sum = 0x01 + 0x08 + 0x04 + BufferID
//             + (StartPage >> 8) + (uint8_t)StartPage
//             + (PageNum >> 8) + (uint8_t)PageNum;
//     SendCheck(sum);
//     data = JudgeStr();    
//     if(data)
//     {
//         ensure = data[9];
//         p->pageID   = (data[10] << 8) + data[11];
//         p->mathscore = (data[12] << 8) + data[13];
//     }
//     else
//         ensure = 0xff;
//     return ensure;
// }

// ===========================================================================
// [已注释] PS_RegModel —— 合并模板原语。已由 Auto_Enroll(31H) 取代
// 需要恢复时，把本段每一行行首的 "// " 删掉即可。
// ===========================================================================
// //合并特征（生成模板）PS_RegModel
// //功能:将CharBuffer1与CharBuffer2中的特征文件合并生成 模板,结果存于CharBuffer1与CharBuffer2
// //说明:  模块返回确认字
// uint8_t IDENTIFIER::PS_RegModel(void)
// {
//     uint16_t sum;
//     uint8_t  ensure;
//     uint8_t  *data = NULL;
//     SendHead();
//     SendAddr();
//     SendFlag(COMMANDSIGN);//命令包标识
//     SendLength(0x03);
//     Sendcmd(0x05);
//     sum = 0x01 + 0x03 + 0x05;
//     SendCheck(sum);
//     data = JudgeStr();    if(data)
//         ensure = data[9];
//     else
//         ensure = 0xff;
//     return ensure;
// }

// ===========================================================================
// [已注释] PS_StoreChar —— 存储模板原语。已由 Auto_Enroll(31H) 取代
// 需要恢复时，把本段每一行行首的 "// " 删掉即可。
// ===========================================================================
// //储存模板 PS_StoreChar
// //功能:将 CharBuffer1 或 CharBuffer2 中的模板文件存到 PageID 号flash数据库位置。
// //参数:  BufferID @ref charBuffer1:0x01	charBuffer1:0x02
// //       PageID（指纹库位置号）
// //说明:  模块返回确认字
// uint8_t IDENTIFIER::PS_StoreChar(uint8_t BufferID, uint16_t PageID)
// {
//     uint16_t sum;
//     uint8_t  ensure;
//     uint8_t  *data = NULL;
//     SendHead();
//     SendAddr();
//     SendFlag(0x01);//命令包标识
//     SendLength(0x06);
//     Sendcmd(0x06);
//     IDUARTwrite_Bytes(BufferID);
//     IDUARTwrite_Bytes(PageID);
//     sum = 0x01 + 0x06 + 0x06 + BufferID
//             + (PageID >> 8) + (uint8_t)PageID;
//     SendCheck(sum);
//     data = JudgeStr();    if(data)
//         ensure = data[9];
//     else
//         ensure = 0xff;
//     return ensure;
// }

//删除模板 PS_DeletChar
//功能:  删除flash数据库中指定ID号开始的N个指纹模板
//参数:  PageID(指纹库模板号)，N删除的模板个数。
//说明:  模块返回确认字
uint8_t IDENTIFIER::PS_DeletChar(uint16_t PageID, uint16_t N)
{
    uint16_t sum;
    uint8_t  ensure;
    uint8_t  *data = NULL;
    SendHead();
    SendAddr();
    SendFlag(0x01);//命令包标识
    SendLength(0x07);
    Sendcmd(0x0C);
    IDUARTwrite_Bytes(PageID);
    IDUARTwrite_Bytes(N);
    sum = 0x01 + 0x07 + 0x0C
            + (PageID >> 8) + (uint8_t)PageID
            + (N >> 8) + (uint8_t)N;
    SendCheck(sum);
    data = JudgeStr();    if(data)
        ensure = data[9];
    else
        ensure = 0xff;
    return ensure;
}

//清空指纹库 PS_Empty
//功能:  删除flash数据库中所有指纹模板
//参数:  无
//说明:  模块返回确认字
uint8_t IDENTIFIER::PS_Empty(void)
{
    uint16_t sum;
    uint8_t  ensure;
    uint8_t  *data = NULL;
    SendHead();
    SendAddr();
    SendFlag(0x01);//命令包标识
    SendLength(0x03);
    Sendcmd(0x0D);
    sum = 0x01 + 0x03 + 0x0D;
    SendCheck(sum);
    data = JudgeStr();    if(data)
        ensure = data[9];
    else
        ensure = 0xff;
    return ensure;
}

//写系统寄存器 PS_WriteReg
//功能:  写模块寄存器
//参数:  寄存器序号RegNum:4\5\6
//说明:  模块返回确认字
uint8_t IDENTIFIER::PS_WriteReg(uint8_t RegNum, uint8_t DATA)
{
    uint16_t sum;
    uint8_t  ensure;
    uint8_t  *data = NULL;
    SendHead();
    SendAddr();
    SendFlag(0x01);//命令包标识
    SendLength(0x05);
    Sendcmd(0x0E);
    IDUARTwrite_Bytes(RegNum);
    IDUARTwrite_Bytes(DATA);
    sum = RegNum + DATA + 0x01 + 0x05 + 0x0E;
    SendCheck(sum);
    data = JudgeStr();    if(data)
        ensure = data[9];
    else
        ensure = 0xff;
    /*
    if(ensure == 0)
        //printf("\r\n设置参数成功！");
    else
        //printf("\r\n%s", EnsureMessage(ensure));
    */
    return ensure;
}

//读系统基本参数 PS_ReadSysPara
//功能:  读取模块的基本参数（波特率，包大小等)
//参数:  无
//说明:  模块返回确认字 + 基本参数（16bytes）
uint8_t IDENTIFIER::PS_ReadSysPara(SysPara *p)
{
    uint16_t sum;
    uint8_t  ensure;
    uint8_t  *data = NULL;
    SendHead();
    SendAddr();
    SendFlag(0x01);//命令包标识
    SendLength(0x03);
    Sendcmd(0x0F);
    sum = 0x01 + 0x03 + 0x0F;
    SendCheck(sum);
    data = JudgeStr();    if(data)
    {
        ensure = data[9];
        p->PS_max = (data[14] << 8) + data[15];
        p->PS_level = data[17];
        p->PS_addr = (data[18] << 24) + (data[19] << 16) + (data[20] << 8) + data[21];
        p->PS_size = data[23];
        p->PS_N = data[25];
    }
    else
        ensure = 0xff;
    if(ensure == 0x00)
    {
        printf("\r\n模块最大指纹容量=%d", p->PS_max);
        printf("\r\n对比等级=%d", p->PS_level);
        //printf("\r\n地址=%x", p->PS_addr);
        printf("\r\n波特率=%d", p->PS_N * 9600);
    }
    else
        printf("\r\n%s", EnsureMessage(ensure));
    return ensure;
}

//设置模块地址 PS_SetAddr
//功能:  设置模块地址
//参数:  PS_addr
//说明:  模块返回确认字
uint8_t IDENTIFIER::PS_SetAddr(uint32_t PS_addr)
{
    uint16_t sum;
    uint8_t  ensure;
    uint8_t  *data = NULL;
    SendHead();
    SendAddr();
    SendFlag(COMMANDSIGN);
    SendLength((uint16_t)0x07);
    Sendcmd((uint8_t)0x15);
    IDUARTwrite_Bytes(PS_addr >> 24);
    IDUARTwrite_Bytes(PS_addr >> 16);
    IDUARTwrite_Bytes(PS_addr >> 8);
    IDUARTwrite_Bytes(PS_addr);
    sum = 0x01 + 0x07 + 0x15
            + (uint8_t)(PS_addr >> 24) + (uint8_t)(PS_addr >> 16)
            + (uint8_t)(PS_addr >> 8) + (uint8_t)PS_addr;
    SendCheck(sum);
    IDaddr = PS_addr; //发送完指令，更换地址
    data = JudgeStr();    if(data)
        ensure = data[9];
    else
        ensure = 0xff;
    IDaddr = PS_addr;
    if(ensure == 0x00)
        printf("\r\n设置地址成功！");
    else
        printf("\r\n%s", EnsureMessage(ensure));
    return ensure;
}

//功能： 模块内部为用户开辟了256bytes的FLASH空间用于存用户记事本,
//	该记事本逻辑上被分成 16 个页。
//参数:  NotePageNum(0~15),Byte32(要写入内容，32个字节)
//说明:  模块返回确认字
uint8_t IDENTIFIER::PS_WriteNotepad(uint8_t NotePageNum, uint8_t *Byte32)
{
    uint16_t sum = 0;
    uint8_t  ensure;
    uint8_t  *data = NULL;
    SendHead();
    SendAddr();
    SendFlag(0x01);//命令包标识
    SendLength(36);
    Sendcmd(0x18);
    IDUARTwrite_Bytes(NotePageNum);
    for(uint8_t i = 0; i < 32; i++)
    {
        IDUARTwrite_Bytes(Byte32[i]);
        sum = sum + Byte32[i];
    }
    sum = 0x01 + 36 + 0x18 + NotePageNum + sum;
    SendCheck(sum);
    data = JudgeStr();    if(data)
        ensure = data[9];
    else
        ensure = 0xff;
    return ensure;
}

//读记事PS_ReadNotepad
//功能：  读取FLASH用户区的128bytes数据
//参数:  NotePageNum(0~15)
//说明:  模块返回确认字+用户信息
uint8_t IDENTIFIER::PS_ReadNotepad(uint8_t NotePageNum, uint8_t *Byte32)
{
    uint16_t sum;
    uint8_t  ensure, i;
    uint8_t  *data = NULL;
    SendHead();
    SendAddr();
    SendFlag(0x01);//命令包标识
    SendLength(0x04);
    Sendcmd(0x19);
    IDUARTwrite_Bytes(NotePageNum);
    sum = 0x01 + 0x04 + 0x19 + NotePageNum;
    SendCheck(sum);
    data = JudgeStr();    if(data)
    {
        ensure = data[9];
        for(i = 0; i < 32; i++)
        {
        Byte32[i] = data[10 + i];
        }
    }
    else
        ensure = 0xff;
    return ensure;
}

// ===========================================================================
// [已注释] PS_HighSpeedSearch —— 高速搜索原语。已由 Auto_Verify(32H) 取代
// 需要恢复时，把本段每一行行首的 "// " 删掉即可。
// ===========================================================================
// //高速搜索PS_HighSpeedSearch
// //功能：以 CharBuffer1或CharBuffer2中的特征文件高速搜索整个或部分指纹库。
// //		  若搜索到，则返回页码,该指令对于的确存在于指纹库中 ，且登录时质量
// //		  很好的指纹，会很快给出搜索结果。
// //参数:  BufferID， StartPage(起始页)，PageNum（页数）
// //说明:  模块返回确认字+页码（相配指纹模板）
// uint8_t IDENTIFIER::PS_HighSpeedSearch(uint8_t BufferID, uint16_t StartPage, uint16_t PageNum, SearchResult *p)
// {
//     uint16_t sum;
//     uint8_t  ensure;
//     uint8_t  *data = NULL;
//     SendHead();
//     SendAddr();
//     SendFlag(0x01);//命令包标识
//     SendLength(0x08);
//     Sendcmd(0x1b);
//     IDUARTwrite_Bytes(BufferID);
//     IDUARTwrite_Bytes(StartPage);
//     IDUARTwrite_Bytes(PageNum);
//     sum = 0x01 + 0x08 + 0x1b + BufferID
//             + (StartPage >> 8) + (uint8_t)StartPage
//             + (PageNum >> 8) + (uint8_t)PageNum;
//     SendCheck(sum);
//     data = JudgeStr();    if(data)
//     {
//         ensure = data[9];
//         p->pageID 	= (data[10] << 8) + data[11];
//         p->mathscore = (data[12] << 8) + data[13];
//     }
//     else
//         ensure = 0xff;
//     return ensure;
// }

//读有效模板个数 PS_ValidsumleteNum
//功能：读有效模板个数
//参数: 无
//说明: 模块返回确认字+有效模板个数ValidN
uint8_t IDENTIFIER::PS_ValidTempleteNum(uint16_t *ValidN)
{
    uint16_t sum;
    uint8_t  ensure;
    uint8_t  *data = NULL;
    SendHead();
    SendAddr();
    SendFlag(0x01);//命令包标识
    SendLength(0x03);
    Sendcmd(0x1d);
    sum = 0x01 + 0x03 + 0x1d;
    SendCheck(sum);
    data = JudgeStr();    if(data)
    {
        ensure = data[9];
        *ValidN = (data[10] << 8) + data[11];
    }
    else
        ensure = 0xff;
    
    if(ensure == 0x00)
    {
        printf("\r\n有效指纹个数=%d", (data[10] << 8) + data[11]);
    }
    else
        printf("\r\n%s", EnsureMessage(ensure));
    return ensure;
}

//与AS608握手 PS_HandShake
//参数: PS_Addr地址指针
//说明: 初始化必做
uint8_t IDENTIFIER::PS_HandShake(uint32_t *PS_Addr)
{
    SendHead();
    SendAddr();
    IDUARTwrite_Bytes(COMMANDSIGN);
    IDUARTwrite_Bytes((uint8_t)0X00);
    IDUARTwrite_Bytes((uint8_t)0X00);
    //vTaskDelay(200 / portTICK_PERIOD_MS);
    size_t uartSize;
    uint8_t data[16];
    ESP_ERROR_CHECK(uart_get_buffered_data_len(UART_NUM_ID, &uartSize));
    uart_read_bytes(UART_NUM_ID, &data, uartSize, 200/portTICK_PERIOD_MS);
    ESP_ERROR_CHECK(uart_flush(UART_NUM_ID));
    if(uartSize != 0) //接收到数据
    {
        if(
        data[0] == 0XEF
        && data[1] == 0X01
        && data[6] == 0X07
        )
        {
        *PS_Addr = (data[2] << 24) + (data[3] << 16)
                    + (data[4] << 8) + (data[5]);
        TouchSleepTimer(*this);           // 收到模块数据，推迟休眠
        return 0;
        }
    }
    return 1;
}

uint8_t IDENTIFIER::PS_Sleep()
{
    uint16_t sum;
    uint8_t  ensure;
    uint8_t *data = NULL;
    
    SendHead();
    SendAddr();
    SendFlag(COMMANDSIGN);
    SendLength(0x03);
    Sendcmd(0x33);          // 休眠指令码
    sum = 0x01 + 0x03 + 0x33;  // 固定校验和（包标识+包长度+指令码）
    SendCheck(sum);
    
    data = JudgeStr();      // 等待应答
    if(data)
        ensure = data[9];
    else
        ensure = 0xff;

    ESP_LOGI(SLEEP_TAG, "休眠指令已发送，应答确认码=0x%02X（0x00 为成功）", ensure);
    return ensure;          // 返回确认码，0x00 表示成功
}

// 呼吸灯指令 PS_ControlBLN（指令码 3CH，普通三色灯包格式，包长度 0007H）
// 功能码：01H 呼吸灯 / 02H 闪烁灯 / 03H 常开灯 / 04H 常闭灯 / 05H 渐开 / 06H 渐灭
// 起始颜色、结束颜色、循环次数各 1 字节。非呼吸灯功能时起始与结束颜色需一致。
// 呼吸灯控制底层包（3CH / 包长度 0007H）
// 参数：功能码(1) 起始颜色(1) 结束颜色(1) 循环次数(1)，颜色码见手册表 3-101
// （bit0=蓝 bit1=绿 bit2=红，故 0x04 是红灯）。
// 手册注：非"呼吸灯"功能时起始颜色要与结束颜色保持一致；循环次数只对
// 呼吸/闪烁/渐开渐灭有效，0x00 表示无限循环。
uint8_t IDENTIFIER::PS_LedCtrlEx(uint8_t func, uint8_t startColor, uint8_t endColor, uint8_t loop)
{
    uint8_t *data;
    uint8_t  ensure;
    uint16_t sum;

    SendHead();
    SendAddr();
    SendFlag(COMMANDSIGN);
    SendLength(0x07);                   // 指令码1 + 参数4 + 校验和2
    Sendcmd(0x3C);                      // 呼吸灯控制指令
    IDUARTwrite_Bytes(func);            // 功能码
    IDUARTwrite_Bytes(startColor);      // 起始颜色
    IDUARTwrite_Bytes(endColor);        // 结束颜色
    IDUARTwrite_Bytes(loop);            // 循环次数

    // 校验和 = 包标识 + 包长度(2B) + 指令码 + 功能码 + 起始色 + 结束色 + 循环次数
    sum = (uint16_t)(0x01 + 0x00 + 0x07 + 0x3C + func + startColor + endColor + loop);
    SendCheck(sum);

    data = JudgeStr();
    ensure = data ? data[9] : 0xff;
    ESP_LOGI(SLEEP_TAG, "LED 控制(功能码=0x%02X 色=0x%02X 循环=%u)应答确认码=0x%02X（0x00 为成功）",
             func, startColor, (unsigned)loop, ensure);
    return ensure;
}

uint8_t IDENTIFIER::PS_LedCtrl(uint8_t func)
{
    return PS_LedCtrlEx(func, ID_LED_OFF, ID_LED_OFF, 0x00);
}

// 注：不提供主动点红灯的接口。验证失败的红灯提示交给模组自己——处于出厂自动模式时
// 它本来就会"验证失败红灯闪三次"（手册 3.5.6）。主控一旦发 3CH 就切到手动模式，
// 反而要把灯的控制权抢过来自己维护，得不偿失。

// 休眠前关灯：常闭灯
uint8_t IDENTIFIER::PS_LedOff(void)
{
    return PS_LedCtrl(0x04);
}

// 唤醒后恢复：呼吸灯自动模式指令 PS_BlnAmSw（指令码 60H，包长度 0004H）
// 功能码 FFH = 开启自动模式，即出厂默认效果（上电蓝灯呼吸，通过绿灯常亮，失败红灯闪三次）。
// 用户选择"唤醒后显式发指令恢复"，这里用 60H/FFH 而不是 3CH/03H 常开灯，
// 因为 60H/FFH 才是模块真正的默认状态；模块掉过电，恢复默认最稳妥。
uint8_t IDENTIFIER::PS_LedAuto(void)
{
    uint8_t *data;
    uint8_t  ensure;
    uint16_t sum;

    SendHead();
    SendAddr();
    SendFlag(COMMANDSIGN);
    SendLength(0x04);                   // 指令码1 + 功能码1 + 校验和2
    Sendcmd(0x60);                      // 呼吸灯自动/手动切换指令
    IDUARTwrite_Bytes((uint8_t)0xFF);   // 功能码：开启自动模式

    sum = (uint16_t)(0x01 + 0x00 + 0x04 + 0x60 + 0xFF);   // = 0x0164
    SendCheck(sum);

    data = JudgeStr();
    ensure = data ? data[9] : 0xff;
    ESP_LOGI(SLEEP_TAG, "LED 自动模式恢复指令应答确认码=0x%02X（0x00 为成功）", ensure);
    return ensure;
}


//模块应答包确认码信息解析
//功能：解析确认码错误信息返回信息
//参数: ensure
const char* IDENTIFIER::EnsureMessage(uint8_t ensure)
{
    const char *p;
    switch(ensure)
    {
    case  0x00:
        p = "       OK       ";
        break;
    case  0x01:
        p = " 数据包接收错误 ";
        break;
    case  0x02:
        p = "传感器上没有手指";
        break;
    case  0x03:
        p = "录入指纹图像失败";
        break;
    case  0x04:
        p = " 指纹太干或太淡 ";
        break;
    case  0x05:
        p = " 指纹太湿或太糊 ";
        break;
    case  0x06:
        p = "  指纹图像太乱  ";
        break;
    case  0x07:
        p = " 指纹特征点太少 ";
        break;
    case  0x08:
        p = "  指纹不匹配    ";
        break;
    case  0x09:
        p = " 没有搜索到指纹 ";
        break;
    case  0x0a:
        p = "   特征合并失败 ";
        break;
    case  0x0b:
        p = "地址序号超出范围";
        break;
    case  0x10:
        p = "  删除模板失败  ";
        break;
    case  0x11:
        p = " 清空指纹库失败 ";
        break;
    case  0x15:
        p = "缓冲区内无有效图";
        break;
    case  0x18:
        p = " 读写FLASH出错  ";
        break;
    case  0x19:
        p = "   未定义错误   ";
        break;
    case  0x1a:
        p = "  无效寄存器号  ";
        break;
    case  0x1b:
        p = " 寄存器内容错误 ";
        break;
    case  0x1c:
        p = " 记事本页码错误 ";
        break;
    case  0x1f:
        p = "    指纹库满    ";
        break;
    case  0x20:
        p = "    地址错误    ";
        break;
    default :
        p = " 返回确认码有误 ";
        break;
    }
    return p;
}

//显示确认码错误信息
void IDENTIFIER::ShowErrMessage(uint8_t ensure)
{
    //OLED_ShowCH(5,0,(uint8_t*)EnsureMessage(ensure));
	printf("%s\r\n",EnsureMessage(ensure));
}
 
 
// ===========================================================================
// [已注释] Add_FR —— 手动注册（按两次手指）。已由 Auto_Enroll(31H) 取代：自动挑 ID、录入 5 次
// 需要恢复时，把本段每一行行首的 "// " 删掉即可。
// ===========================================================================
// //录指纹
// void IDENTIFIER::Add_FR(void)
// {
//     uint8_t i = 0;
//     uint8_t ensure, processnum = 0;
//     uint8_t ID_NUM = 0;
//     while(1)
//     {
//         switch (processnum)
//         {
//         case 0:
//         i++;
//                 printf("请按手指\r\n");
//         ensure = PS_GetImage();
//         if(ensure == 0x00)
//         {
//             ensure = PS_GenChar(CharBuffer1); //生成特征
//             if(ensure == 0x00)
//             {
//                         printf("指纹正常\r\n");
//             i = 0;
//             processnum = 1; //跳到第二步
//             }
//             else ShowErrMessage(ensure);
//         }
//         else ShowErrMessage(ensure);
//         break;
//     
//         case 1:
//         i++;
//             printf("请再按一次\r\n");
//         ensure = PS_GetImage();
//         if(ensure == 0x00)
//         {
//             ensure = PS_GenChar(CharBuffer2); //生成特征
//             if(ensure == 0x00)
//             {
//                         printf("指纹正常\r\n");
//             i = 0;
//             processnum = 2; //跳到第三步
//             }
//             else ShowErrMessage(ensure);
//         }
//         else ShowErrMessage(ensure);
//         break;
//     
//         case 2:
//             printf("对比两次指纹\r\n");
//         ensure = PS_Match();
//         if(ensure == 0x00)
//         {
//                     printf("对比成功\r\n");
//             processnum = 3; //跳到第四步
//         }
//         else
//         {
//                     printf("对比失败\r\n");
//             ShowErrMessage(ensure);
//             i = 0;
//             processnum = 0; //跳回第一步
//         }
//         vTaskDelay(500 / portTICK_PERIOD_MS);
//         break;
//     
//         case 3:
//             printf("生成指纹模板\r\n");
//         vTaskDelay(500 / portTICK_PERIOD_MS);
//         ensure = PS_RegModel();
//         if(ensure == 0x00)
//         {
//                     printf("生成指纹模板成功\r\n");
//             processnum = 4; //跳到第五步
//         }
//         else
//         {
//             processnum = 0;
//             ShowErrMessage(ensure);
//         }
//         vTaskDelay(1000 / portTICK_PERIOD_MS);
//         break;
//     
//         case 4:
//                 printf("默认选择ID为1 \r\n");
//             ID_NUM = 1;
//             #if 0
//         while(key_num != 3)
//         {
//             key_num = KEY_Scan(0);
//             if(key_num == 2)
//             {
//             key_num = 0;
//             if(ID_NUM > 0)
//                 ID_NUM--;
//             }
//             if(key_num == 4)
//             {
//             key_num = 0;
//             if(ID_NUM < 99)
//                 ID_NUM++;
//             }
//             OLED_ShowCH(40, 6, "ID=");
//             OLED_ShowNum(65, 6, ID_NUM, 2, 1);
//         }
//             
//         key_num = 0;
//                 #endif
//         ensure = PS_StoreChar(CharBuffer2, ID_NUM); //储存模板
//         if(ensure == 0x00)
//         {
//                     printf("录入指纹成功\r\n");
//             vTaskDelay(1500 / portTICK_PERIOD_MS);
//             return ;
//         }
//         else
//         {
//             processnum = 0;
//             ShowErrMessage(ensure);
//         }
//         break;
//         }
//         vTaskDelay(400 / portTICK_PERIOD_MS);
//         if(i == 10) //超过5次没有按手指则退出
//         {
//         break;
//         }
//     }
// }
 
//SysPara AS608Para;//指纹模块AS608参数

// ===========================================================================
// [已注释] press_FR —— 手动验证循环（GetImage+GenChar+HighSpeedSearch）。已由 Auto_Verify(32H) 取代
// 需要恢复时，把本段每一行行首的 "// " 删掉即可。
// ===========================================================================
// //刷指纹
// //自动识别匹配
// //功能:采图 -> 生成特征 -> 高速搜索，命中指纹库就返回
// //参数:timeout_ms 最长等待时间，超过它仍没识别到就返回
// //返回值:识别成功 true，超时 false
// //说明:原实现是 while(1) 死循环，触摸唤醒进来后永远回不到主循环，
// //     "60 秒无数据即休眠"再也不生效，低功耗循环只能走一轮。
// bool IDENTIFIER::press_FR(uint32_t timeout_ms)
// {
//     SearchResult seach;
//     uint8_t  ensure;
//     int64_t  deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
// 
//     while (esp_timer_get_time() < deadline)
//     {
//         ensure = PS_GetImage();
//         if (ensure != 0x00)
//         {
//             // 0x02 = 传感器上没有手指，属正常等待；其余值是真错误，必须打出来，
//             // 尤其 0xFF 表示模块压根没应答（供电/接线/波特率问题），别把它当成"没按手指"糊过去
//             if (ensure == 0x02){
//                 printf("请按手指\r\n");
//             } else {
//                 printf("采集图像失败，确认码=0x%02X\r\n", ensure);
//             }
//             vTaskDelay(100 / portTICK_PERIOD_MS);
//             continue;
//         }
// 
//         ensure = PS_GenChar(CharBuffer1);
//         if (ensure != 0x00)
//         {
//             // 图像质量不达标（手指太干/太湿/移动），直接重采，不消耗重试间隔
//             printf("生成特征失败，确认码=0x%02X\r\n", ensure);
//             continue;
//         }
// 
//         ensure = PS_HighSpeedSearch(CharBuffer1, 0, 99, &seach);
//         if (ensure == 0x00) //搜索成功
//         {
//             printf("指纹验证成功 ID:%u 得分:%u\r\n",
//                    (unsigned)seach.pageID, (unsigned)seach.mathscore);
//             return true;                    // 识别成功，交还主循环重新计时
//         }
// 
//         printf("验证失败，确认码=0x%02X\r\n", ensure);
//         vTaskDelay(1500 / portTICK_PERIOD_MS);  // 歇一下，避免连续空打
//     }
// 
//     printf("识别超时，%u 毫秒内未识别到指纹\r\n", (unsigned)timeout_ms);
//     return false;
// }

// =====================================================================
// 自动注册 / 自动验证（手册 3.3.2 模块指令集，指令码 30H/31H/32H）
// =====================================================================

//自动注册确认码释义（手册表 3-52，对应指令 31H PS_AutoEnroll）
static const char *EnrollAckMsg(uint8_t ack)
{
    switch (ack)
    {
    case 0x00: return "成功";
    case 0x01: return "失败";
    case 0x07: return "生成特征失败";
    case 0x0a: return "合并模板失败";
    case 0x0b: return "ID 号超出范围";
    case 0x1f: return "指纹库已满";
    case 0x22: return "指纹模板非空";
    case 0x25: return "录入次数设置错误";
    case 0x26: return "超时";
    case 0x27: return "指纹已存在";
    default:   return "未知确认码";
    }
}

//自动验证确认码释义（手册表 3-55，对应指令 32H PS_AutoIdentify）
//注意 09H 的字面释义是"没搜索到指纹"，但手册指令说明第 6)、7) 条写明
//"生成特征失败"和"比对失败"同样返回 09 05H——所以它实际是"这一次没认出来"
//的统一码，调用方按"未知指纹"处理即可。
static const char *VerifyAckMsg(uint8_t ack)
{
    switch (ack)
    {
    case 0x00: return "成功";
    case 0x01: return "失败";
    case 0x07: return "生成特征失败";
    case 0x09: return "没搜索到指纹（未知指纹）";
    case 0x0b: return "ID 号超出范围";
    case 0x17: return "残留指纹";
    case 0x23: return "指纹模板为空";
    case 0x24: return "指纹库为空";
    case 0x26: return "超时";
    case 0x27: return "指纹已存在";
    default:   return "未知确认码";
    }
}

//按长度精确读一个完整应答包
//为什么不能沿用 JudgeStr()：那个函数是"先干等 500ms，再看缓冲区里有没有包头"，
//只够对付一问一答的单包指令。自动注册会连续吐出十几个进度包（采图/生成特征/手指离开
//每轮三个，最后还有合并、查重、存储），用 JudgeStr 只抓得到第一个，后面的进度和
//决定成败的"模板存储结果"全丢了，也就无从判断注册到底成没成。
//应答包结构：EF 01 | 地址(4) | 包标识(1) | 包长度(2) | 确认码(1) 参数(n) 校验和(2)
//其中"包长度"计的是从确认码到校验和的字节数，故参数区长度 = 包长度 - 3。
bool IDENTIFIER::ReadAckPacket(AckPacket *pkt, uint32_t timeout_ms)
{
    const uint8_t tail_len = 7;         // 地址(4) + 包标识(1) + 包长度(2)
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    uint8_t  b;
    uint8_t  tail[7];
    uint16_t plen;
    bool     synced = false;

    pkt->param_len = 0;

    // 1) 同步包头 EF 01。途中任何不匹配的字节都丢掉重找，
    //    这样即便上一次读包读歪了也能自动纠正回来，不会一路错到底。
    while (!synced && esp_timer_get_time() < deadline)
    {
        if (uart_read_bytes(UART_NUM_ID, &b, 1, pdMS_TO_TICKS(50)) != 1)
            continue;
        if (b != 0xEF)
            continue;
        if (uart_read_bytes(UART_NUM_ID, &b, 1, pdMS_TO_TICKS(50)) != 1)
            continue;
        if (b == 0x01)
            synced = true;
    }
    if (!synced)
        return false;

    // 2) 地址 + 包标识 + 包长度
    if (uart_read_bytes(UART_NUM_ID, tail, tail_len, pdMS_TO_TICKS(200)) != (int)tail_len)
        return false;
    plen = ((uint16_t)tail[5] << 8) | tail[6];

    // 包长度至少要装得下 确认码(1) + 校验和(2)
    if (plen < 3 || (uint16_t)(plen - 3) > sizeof(pkt->param))
        return false;

    // 3) 确认码 + 参数区 + 校验和，一次性读完
    if (uart_read_bytes(UART_NUM_ID, pkt->param, plen, pdMS_TO_TICKS(500)) != (int)plen)
        return false;

    TouchSleepTimer(*this);             // 收到模块数据，推迟休眠

    // param[] 里现在装的是 [确认码, 参数..., 校验和2]，
    // 把确认码取出来、参数区整体前移一位，后面调用方就只管参数了。
    pkt->ack = pkt->param[0];
    pkt->param_len = (uint8_t)(plen - 3);
    if (pkt->param_len > 0)
        memmove(pkt->param, pkt->param + 1, pkt->param_len);
    return true;
}

//取消指令 PS_Cancel（30H）：中止正在进行的自动注册或自动验证
uint8_t IDENTIFIER::PS_Cancel(void)
{
    AckPacket pkt;

    SendHead();
    SendAddr();
    SendFlag(COMMANDSIGN);
    SendLength(0x03);
    Sendcmd(0x30);
    SendCheck((uint16_t)(0x01 + 0x00 + 0x03 + 0x30));   // = 0x0034

    if (!ReadAckPacket(&pkt, 1000))
        return 0xff;

    printf("取消指令应答确认码=0x%02X（0x00 为成功）\r\n", pkt.ack);
    return pkt.ack;
}

//读索引表 PS_ReadIndexTable（1FH）
//功能:读录入模板的索引表，每 1 bit 对应一枚模板，1=已录入、0=空
//参数:page 索引表页码，第 0 页覆盖模板 0~255（100 枚的库够用）
//      index32 出参，32 字节 = 256 bit
//返回:确认码，0x00 为成功
uint8_t IDENTIFIER::PS_ReadIndexTable(uint8_t page, uint8_t *index32)
{
    AckPacket pkt;

    SendHead();
    SendAddr();
    SendFlag(COMMANDSIGN);
    SendLength(0x04);
    Sendcmd(0x1F);
    IDUARTwrite_Bytes(page);
    SendCheck((uint16_t)(0x01 + 0x00 + 0x04 + 0x1F + page));

    if (!ReadAckPacket(&pkt, 2000))
        return 0xff;

    if (pkt.ack == 0x00 && index32 && pkt.param_len >= 32)
        memcpy(index32, pkt.param, 32);
    return pkt.ack;
}

//找下一个空闲的模板 ID
//功能:自动注册要指定 ID，这里替调用方挑一个没被占用的
//说明:先读索引表精确定位；索引表读不到就退回 start，交给模块自己的
//     "合法性检测"逐个试探（Auto_Enroll 里有对应的重试逻辑）
//返回:空闲 ID；0xFFFF 表示指纹库已满
uint16_t IDENTIFIER::FindFreeID(uint16_t start, uint16_t end)
{
    uint8_t index[32];

    if (PS_ReadIndexTable(0, index) == 0x00)
    {
        for (uint16_t id = start; id <= end && id < 256; id++)
        {
            // 位序按 LSB 在前解释（AS608 系惯例），文档未明确写明；
            // 万一猜反了 Auto_Enroll 会靠模块返回的 22H 自动换下一个 ID。
            if (((index[id >> 3] >> (id & 0x07)) & 0x01) == 0)
                return id;
        }
        return 0xffff;
    }

    ESP_LOGW(ENROLL_TAG, "读索引表失败，退化为从 ID %u 开始试探", (unsigned)start);
    return start;
}

//组装自动注册模板指令包 PS_AutoEnroll（31H）
//包格式:包头2 地址4 包标识1 包长度0008H 指令码31H ID号2 录入次数1 参数2 校验和2
void IDENTIFIER::SendAutoEnrollCmd(uint16_t id, uint8_t times, uint16_t params)
{
    SendHead();
    SendAddr();
    SendFlag(COMMANDSIGN);
    SendLength(0x08);
    Sendcmd(0x31);
    IDUARTwrite_Bytes(id);              // ID 号，高字节在前
    IDUARTwrite_Bytes(times);           // 录入次数，1 字节
    IDUARTwrite_Bytes(params);          // 参数，高字节在前
    // 校验和 = 包标识 + 包长度(2B) + 指令码 + ID号(2B) + 录入次数 + 参数(2B)
    SendCheck((uint16_t)(0x01 + 0x00 + 0x08 + 0x31
                         + (id >> 8) + (uint8_t)id
                         + times
                         + (params >> 8) + (uint8_t)params));
}

//自动注册模板 PS_AutoEnroll（31H）
//功能:自动挑一个空闲 ID，连续录入 times 次后由模块合并成模板并存库
//参数:outID 出参，回填实际使用的 ID（可传 nullptr）
//      times 录入次数，手册规定 2~10
//返回:true 表示模板已成功存储
bool IDENTIFIER::Auto_Enroll(uint16_t *outID, uint8_t times, uint32_t timeout_ms)
{
    // 参数位：bit0=1 采图后灭灯；bit4=1 不允许重复指纹注册；
    // bit2 保持 0 —— 必须让模块回关键步骤，否则收不到进度、也不知道何时结束
    const uint16_t params = ID_ENROLL_P_LEDOFF | ID_ENROLL_P_NODUP;
    AckPacket pkt;
    bool accepted = false;

    if (times < 2 || times > 10)
    {
        printf("录入次数 %u 不合法，手册规定 2~10 次\r\n", (unsigned)times);
        return false;
    }

    uint16_t id = FindFreeID(0, ID_TEMPLATE_MAX - 1);
    if (id == 0xffff)
    {
        printf("指纹库已满，无法注册\r\n");
        return false;
    }

    // 阶段一：让模块认下一个可用 ID。
    // 索引表的字节内位序文档没写明，万一猜错会挑到已占用的 ID，此时模块在
    // "合法性检测"阶段返回 22H，顺势换下一个继续试，最多试满整个库。
    for (uint16_t attempts = 0; attempts < ID_TEMPLATE_MAX; attempts++)
    {
        SendAutoEnrollCmd(id, times, params);

        if (!ReadAckPacket(&pkt, 3000))
        {
            printf("自动注册无应答，检查模块通信\r\n");
            return false;
        }
        if (pkt.param_len < 2)
        {
            printf("ID %u 的应答格式异常（参数区只有 %u 字节）\r\n",
                   (unsigned)id, (unsigned)pkt.param_len);
            return false;
        }

        if (pkt.param[0] != 0x00)
        {
            // 不是合法性检测包，说明模块已直接进入录入流程，照单全收
            accepted = true;
            break;
        }
        if (pkt.ack == 0x00)                // 00 00 00 合法性检测通过
        {
            accepted = true;
            break;
        }
        if (pkt.ack == 0x22)                // 该 ID 已有模板，换下一个
        {
            if (++id >= ID_TEMPLATE_MAX)
            {
                printf("指纹库已满，无法注册\r\n");
                return false;
            }
            continue;
        }
        printf("注册前置检查失败：确认码=0x%02X %s\r\n", pkt.ack, EnrollAckMsg(pkt.ack));
        return false;
    }
    if (!accepted)
    {
        printf("试遍整个指纹库都找不到可用 ID，注册失败\r\n");
        return false;
    }

    // 阶段二：读进度包，直到模块报"模板存储结果"
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    printf("ID %u 开始自动注册，需要按 %u 次手指\r\n", (unsigned)id, (unsigned)times);

    while (esp_timer_get_time() < deadline)
    {
        if (!ReadAckPacket(&pkt, 3000))
            continue;                       // 这一轮没包，继续等
        if (pkt.param_len < 2)
            continue;

        uint8_t step = pkt.param[0];        // 参数1：当前处于哪一步
        uint8_t code = pkt.param[1];        // 参数2：第 n 次录入 / F0 F1 F2

        if (pkt.ack != 0x00)
        {
            // 07 = 生成特征失败，手册要求退回"等待采图"重来，不算致命
            if (pkt.ack == 0x07 && step == 0x02)
            {
                printf("第 %u 次特征生成失败，请重新按下手指\r\n", (unsigned)code);
                continue;
            }
            printf("注册中断：步骤=0x%02X 确认码=0x%02X %s\r\n",
                   step, pkt.ack, EnrollAckMsg(pkt.ack));
            PS_Cancel();                    // 别把模块留在半途的注册状态
            return false;
        }

        switch (step)
        {
        case 0x01: printf("第 %u 次：请按手指\r\n", (unsigned)code); break;
        case 0x02: printf("第 %u 次：特征已生成，请抬起手指\r\n", (unsigned)code); break;
        case 0x03: printf("第 %u 次：录入成功\r\n", (unsigned)code); break;
        case 0x04: printf("正在合并模板...\r\n"); break;
        case 0x05: printf("重复指纹检查通过\r\n"); break;
        case 0x06:
            printf("自动注册完成，指纹已存入 ID %u\r\n", (unsigned)id);
            if (outID) *outID = id;
            return true;
        default:
            printf("未知注册步骤 0x%02X，已忽略\r\n", step);
            break;
        }
    }

    printf("自动注册超时（%u 毫秒）\r\n", (unsigned)timeout_ms);
    PS_Cancel();
    return false;
}

//自动验证指纹 PS_AutoIdentify（32H）
//功能:一站式验证，模块自己完成采图、生成特征、搜索
//参数:outID/outScore 出参，回填匹配到的模板号与得分（可传 nullptr）
//      id 填 ID_VERIFY_ALL_ID(0xFFFF) 做 1:N 搜索，填具体 ID 则与该模板 1:1 匹配
//      level 分数等级 0~9，越大越严格
//返回:验证通过 true
bool IDENTIFIER::Auto_Verify(uint16_t *outID, uint16_t *outScore,
                             uint16_t id, uint8_t level, uint32_t timeout_ms)
{
    const uint16_t params = ID_VERIFY_P_LEDOFF;   // bit0=1 采图后灭灯；bit2=0 要求返回关键步骤
    AckPacket pkt;

    SendHead();
    SendAddr();
    SendFlag(COMMANDSIGN);
    SendLength(0x08);
    Sendcmd(0x32);
    IDUARTwrite_Bytes(level);           // 分数等级，1 字节
    IDUARTwrite_Bytes(id);              // ID 号，高字节在前；0xFFFF 表示 1:N
    IDUARTwrite_Bytes(params);          // 参数，高字节在前
    // 校验和 = 包标识 + 包长度(2B) + 指令码 + 分数等级 + ID号(2B) + 参数(2B)
    SendCheck((uint16_t)(0x01 + 0x00 + 0x08 + 0x32 + level
                         + (id >> 8) + (uint8_t)id
                         + (params >> 8) + (uint8_t)params));

    printf("自动验证开始(%s)，请按手指\r\n",
           (id == ID_VERIFY_ALL_ID) ? "1:N 搜索" : "1:1 匹配");

    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline)
    {
        if (!ReadAckPacket(&pkt, 3000))
            continue;
        if (pkt.param_len < 5)
            continue;

        // 应答参数区：参数(1) ID号(2) 得分(2)
        uint8_t  step  = pkt.param[0];
        uint16_t rid   = ((uint16_t)pkt.param[1] << 8) | pkt.param[2];
        uint16_t score = ((uint16_t)pkt.param[3] << 8) | pkt.param[4];

        if (step == 0x00)                   // 合法性检测
        {
            if (pkt.ack != 0x00)
            {
                // 24H 指纹库为空 / 0bH ID 号无效 / 23H 模板不存在
                printf("自动验证指令被拒，确认码=0x%02X %s\r\n",
                       pkt.ack, VerifyAckMsg(pkt.ack));
                return false;
            }
            continue;
        }
        if (step == 0x01)                   // 采图结果
        {
            if (pkt.ack != 0x00)
            {
                // 26H 超时（没按或按得太浅）/ 17H 残留指纹（上次没抬干净）
                printf("采图失败，确认码=0x%02X %s\r\n", pkt.ack, VerifyAckMsg(pkt.ack));
                return false;
            }
            continue;
        }
        if (step == 0x05)                   // 搜索结果
        {
            if (pkt.ack == 0x00)
            {
                printf("验证通过，ID=%u 得分=%u\r\n", (unsigned)rid, (unsigned)score);
                if (outID)    *outID = rid;
                if (outScore) *outScore = score;
                return true;
            }
            if (pkt.ack == 0x09)
                // 09H 字面是"没搜索到指纹"，但手册指令说明第 6)、7) 条写明
                // "生成特征失败"和"比对失败"也返回 09 05H，统一按未知指纹处理。
                // 红灯提示交给模组自己：出厂自动模式下验证失败它会红灯闪三次。
                printf("未知指纹，确认码=0x%02X %s\r\n", pkt.ack, VerifyAckMsg(pkt.ack));
            else
                printf("验证不通过，确认码=0x%02X %s\r\n", pkt.ack, VerifyAckMsg(pkt.ack));
            return false;
        }
    }

    printf("自动验证超时（%u 毫秒）\r\n", (unsigned)timeout_ms);
    PS_Cancel();
    return false;
}
 
//删除单个指纹
void IDENTIFIER::Del_FR(void)
{
    uint8_t  ensure;
    uint16_t ID_NUM = 0;
        printf("单个删除指纹开始，默认删除ID为1");
        ID_NUM = 1;
    ensure = PS_DeletChar(ID_NUM, 1); //删除单个指纹
    if(ensure == 0)
    {
        printf("删除指纹成功 \r\n");
    }
    else
        ShowErrMessage(ensure);
    vTaskDelay(1500 / portTICK_PERIOD_MS);
    
    }
    /*清空指纹库*/
    void IDENTIFIER::Del_FR_Lib(void)
    {
        uint8_t  ensure;
        printf("删除指纹库开始\r\n");
    ensure = PS_Empty(); //清空指纹库
    if(ensure == 0)
    {
            printf("清空指纹库成功\r\n");
    }
    else
        ShowErrMessage(ensure);
    vTaskDelay(1500 / portTICK_PERIOD_MS);
}

//按 ID 删除单枚模板（0CH）
//给串口删除控制台用：那边已经把 ID 解析好了，这里只负责发指令 + 译确认码。
bool IDENTIFIER::Del_FR_ID(uint16_t id)
{
    // 容量之外的 ID 模块也不会删掉什么，但与其让它回一个含混的确认码，
    // 不如在本地就拦下来，顺便把"库容量是 100"这件事讲清楚。
    if (id >= ID_TEMPLATE_MAX)
    {
        printf("ID %u 超出指纹库容量(%u)\r\n", (unsigned)id, (unsigned)ID_TEMPLATE_MAX);
        return false;
    }

    uint8_t ensure = PS_DeletChar(id, 1);
    if (ensure == 0x00)
        return true;

    // 22H=模板非空、24H=库为空之类的都交给统一译码，避免这里再抄一份表
    ShowErrMessage(ensure);
    return false;
}

uint32_t IDENTIFIER::PS_GetRandomCode()
{
    uint8_t  ensure;
    uint8_t *data = NULL;
    uint32_t randomCode = 0;
    SendHead();
    SendAddr();
    SendFlag(COMMANDSIGN);
    SendLength(0x03);
    Sendcmd(0x14);
    SendCheck(0x18);
    data = JudgeStr();    
    if(data)
    {
        ensure = data[9];
        randomCode = (data[10] << 24)+
                     (data[11] << 16)+
                     (data[12] << 8)+
                      data[13];
    }
    else
        ensure = 0x01;
    
    if(ensure == 0x00)
    {
        #ifdef TEST
        printf("\r随机数%lx\n", randomCode);
        #endif
    }
    else
    {
        #ifdef TEST
        printf("\r\n%s", EnsureMessage(ensure));
        #endif
    }
    return randomCode;
}

// 倒计时到点后的回调：直接执行完整休眠流程。
// arg 就是 ZW_Sleep() 注册定时器时传进来的 IDENTIFIER*，回调自己就能收尾，
// 不需要再往 app_main 递一个"请求休眠"的标志来回倒手。
//
// 两个必须知道的代价：
//  ① 本回调跑在 esp_timer 服务任务上，EnterDeepSleep() 会一路阻塞几百毫秒
//     （JudgeStr 收一次包就要 500ms），期间 esp_timer 派发不了别的定时器。
//     反正芯片马上就要复位重跑，这个代价可以接受。
//  ② 用的也是 esp_timer 任务的栈，所以 sdkconfig 里
//     CONFIG_ESP_TIMER_TASK_STACK_SIZE 已从默认 3584 提到 5120。
void PS_Sleep(void *arg)
{
    if (arg == nullptr){
        ESP_LOGE(SLEEP_TAG, "休眠回调没拿到 IDENTIFIER 指针，放弃休眠");
        return;
    }
    ESP_LOGI(SLEEP_TAG, "计时到点，开始休眠");
    static_cast<IDENTIFIER *>(arg)->EnterDeepSleep();   // 不返回
}

// 完整的断电休眠流程。进入 Deep-sleep 后芯片复位重跑 app_main，本函数不返回。
void IDENTIFIER::EnterDeepSleep(void)
{
    ESP_LOGI(SLEEP_TAG, "===== 开始休眠流程 =====");

    s_sending_sleep = true;                 // 关掉"收到数据就续命"的闸门
    if (sleep_timer){
        // 正常路径下这里是从该定时器自己的回调里调用的：一次性定时器在触发瞬间
        // 就已从队列摘除、alarm 归零，所以 esp_timer_stop 会返回 ESP_ERR_INVALID_STATE。
        // 属正常，返回值故意不检查。若从别处调用（如 ZW_Sleep(0, zw)），
        // 这一句才真正起到"掐掉倒计时"的作用。
        esp_timer_stop(sleep_timer);
    }

    PS_LedOff();                            // 1) 关掉模组 LED（3CH/04H 常闭灯）
    vTaskDelay(pdMS_TO_TICKS(100));         //    给应答留出收包时间
    PS_Sleep();                             // 2) 模组自身进休眠（33H），静态功耗约 10µA
    vTaskDelay(pdMS_TO_TICKS(100));

    // 3) 切断模组 VCC。gpio_hold_en 把 IO7 钉在低电平，gpio_deep_sleep_hold_en
    //    让这个电平在 Deep-sleep 期间也保持住（数字域掉电，不 hold 的话引脚会浮空）。
    ID_PowerOff();
    gpio_hold_en(ID_VCC_GPIO);
    gpio_deep_sleep_hold_en();

    // 4) MCU 的 TX 改成高阻并保持，避免模组断电后电流从 TX 倒灌进模组的 VCC 轨。
    gpio_set_direction(UART_NUM_ID_TX, GPIO_MODE_INPUT);
    gpio_hold_en(UART_NUM_ID_TX);

    // 5) 配置 TOUCH_OUT 为唤醒源。模组的 V_SENSOR(PIN1) 必须常供电，
    //    手指触摸时 TOUCH_OUT 输出高电平，把 ESP32-C3 从 Deep-sleep 拉起来。
    //
    //    这里用的是 GPIO 唤醒而不是 EXT0：ESP32-C3 没有 RTC IO
    //    （SOC_RTCIO_PIN_COUNT = 0），SOC_PM_SUPPORT_EXT0_WAKEUP 也压根没定义，
    //    esp_sleep_enable_ext0_wakeup() 在 C3 上不可用。
    //    掩码里只有 GPIO0~GPIO5 合法，越界会返回 ESP_ERR_INVALID_ARG。
    //
    //    该 API 自己不碰引脚配置，真正的上下拉是 esp_deep_sleep_start() 内部按唤醒
    //    电平装的（ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS 默认开）——高电平唤醒
    //    对应内部下拉，所以没有触摸时这个脚不会被拉高、不会误唤醒。
    ESP_ERROR_CHECK(esp_deep_sleep_enable_gpio_wakeup(1ULL << ID_TOUCH_OUT_GPIO,
                                                      ESP_GPIO_WAKEUP_GPIO_HIGH));
    ESP_LOGI(SLEEP_TAG, "已配置 TOUCH_OUT(GPIO%d) 高电平唤醒，即将进入 Deep-sleep",
             (int)ID_TOUCH_OUT_GPIO);

    esp_deep_sleep_start();                 // 不返回，唤醒后从 app_main 重新开始
}

// 轮询 TOUCH_OUT，判断当前有没有手指按在传感器上（高电平=活体检测为真）。
//
// 这里的一次性 GPIO 配置不能省：全项目只有 EnterDeepSleep() 里的
// esp_deep_sleep_enable_gpio_wakeup() 碰过这个脚，而那个 API 明确写了
// "does not modify pin configuration"，且那条路径只有"准备休眠"时才会走到。
// 冷启动时它还是上电复位状态，而 ESP-IDF 明确规定——pad 没有配置成输入时
// gpio_get_level() 恒返回 0。结果就是首次上电后主循环永远看不到触摸，
// 表现为"按指纹没反应"；而触摸唤醒那条路走的是 from_touch 分支直接调
// Auto_Verify()，压根不读这个脚，所以唤醒后反而是正常的。
//
// 下拉：规格书说 TOUCH_OUT 是推挽输出、电压与触控电压一致，本不需要上下拉；
// 但模组刚上电/掉电瞬间这个脚可能呈高阻，挂个下拉能保证读到的是 0 而不是浮空值。
bool IDENTIFIER::Is_Touch(void)
{
    static bool s_touch_gpio_ready = false;
    if (!s_touch_gpio_ready){
        gpio_config_t cfg = {};
        cfg.pin_bit_mask = 1ULL << ID_TOUCH_OUT_GPIO;
        cfg.mode         = GPIO_MODE_INPUT;
        cfg.pull_up_en   = GPIO_PULLUP_DISABLE;
        cfg.pull_down_en = GPIO_PULLDOWN_ENABLE;
        cfg.intr_type    = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&cfg));
        s_touch_gpio_ready = true;
        ESP_LOGI(SLEEP_TAG, "TOUCH_OUT(GPIO%d) 已配置为输入", (int)ID_TOUCH_OUT_GPIO);
    }
    return gpio_get_level(ID_TOUCH_OUT_GPIO);
}

bool is_sleep_init=false;
void ZW_Sleep(uint8_t t, IDENTIFIER &zw)
{
    // t == 0 表示立刻休眠
    if (t == 0){
        PS_Sleep(&zw);
        return;
    }

    if (!is_sleep_init){
        esp_timer_create_args_t sleep_timer_args = {
            .callback = PS_Sleep,
            .arg = &zw,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "sleep_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&sleep_timer_args,&sleep_timer));
        is_sleep_init = true;
    }

    // 这里必须用 stop + start_once，不能用 esp_timer_restart：
    // restart 内部要求 timer_armed()（即 alarm > 0），而一次性定时器一旦触发过，
    // timer_remove() 就把 alarm 清成 0 了，此时 restart 只会返回 ESP_ERR_INVALID_STATE
    // 且什么都不做，导致定时器再也装填不上、模块永远不休眠。
    esp_timer_stop(sleep_timer);   // 未装填时返回 ESP_ERR_INVALID_STATE，属正常，忽略
    ESP_ERROR_CHECK(esp_timer_start_once(sleep_timer, (uint64_t)t * 1000000ULL));

    ESP_LOGI(SLEEP_TAG, "休眠倒计时重置为 %u 秒", (unsigned)t);
}

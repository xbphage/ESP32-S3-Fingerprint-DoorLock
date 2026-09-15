#ifndef _delete_cli_HPP
#define _delete_cli_HPP
#pragma once

#include "fingerID.hpp"

// 串口删除控制台。
//
// 用途：设备插上电脑就能删指纹，不用为了删一条记录去改代码重烧。
// 支持的命令见 delete_cli.cpp 里的 cli_help()。
//
// 用法：在主循环里周期性地调一次即可。函数内部是非阻塞读串口的，
//       只有真的收到一整行命令时才同步执行（执行期间会短暂占用主循环，
//       和 open_door() 一样，属于正常现象）。
//
// 为什么是轮询而不是独立的 FreeRTOS 任务：
//   指纹模组只有一路 UART，主循环和删除操作都要用它。开独立任务就必须引入
//   互斥锁，还要额外处理"主循环正卡在 Auto_Verify() 里最长 15 秒"时的竞争。
//   挂在主循环里顺序执行，天然没有并发问题，代价只是删除期间不响应触摸。
void DeleteCLI_Poll(IDENTIFIER &zw);

#endif

#ifndef __UI_LIMITS_H__
#define __UI_LIMITS_H__

// ---- 待办(todo) 抓取 / 显示上限（CODE_REVIEW P2 #26 统一常量）----
//
// 原先两个文件各自写死且数值不一致：
//   - managers/todo_mcp_client.cc : kMaxRows = 10（一次最多抓取 10 条）
//   - custom_lcd_display.cc       : kMaxItems = 6（待办卡一屏最多渲染 6 条）
// 超过 6 条时第 7~10 条「只抓取、不上屏」，此前没有任何说明，容易被当成静默丢数据。
// 现在统一到此头文件，两处都引用下面的常量，并明确这条已知策略：
//   * 抓取侧保留 10 条缓冲，服务端排序/分页小幅变化时首屏条目不会抖动；
//   * 显示侧受待办卡片高度限制，最多渲染 6 条（第 7~10 条为已知的上屏裁剪，非丢失）。
// 如需调整，改这里即可同时影响抓取与显示。
constexpr int kTodoFetchMaxRows   = 10;   // 从 MCP 一次最多抓取的待办条数
constexpr int kTodoDisplayMaxItems = 6;   // 待办卡片一屏最多渲染的条数

#endif  // __UI_LIMITS_H__

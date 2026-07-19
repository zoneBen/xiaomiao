#ifndef GAME_MENU_H
#define GAME_MENU_H

#include <stdint.h>
#include <stdbool.h>
#include "chess_ui.h"  // 重用 ChessScreen 枚举和 KeyCode

// 通用游戏菜单状态
typedef enum {
    GAME_MENU_STATE_MAIN,          // 主菜单
    GAME_MENU_STATE_BLUETOOTH,     // 蓝牙菜单
    GAME_MENU_STATE_SPLASH,        // 启动画面
    GAME_MENU_STATE_GAME,          // 游戏中
    GAME_MENU_STATE_EXIT_CONFIRM,  // 退出确认
} GameMenuState;

// 通用游戏菜单回调
typedef struct GameMenuCallbacks {
    void (*on_local_game)(void);    // 开始本地游戏
    void (*on_create_game)(void);   // 创建蓝牙游戏
    void (*on_join_game)(void);     // 加入蓝牙游戏
    void (*on_back_to_selector)(void); // 返回游戏选择器
    void (*render_custom)(void);    // 自定义渲染
} GameMenuCallbacks;

// 通用游戏菜单配置
typedef struct GameMenuConfig {
    const char* game_name;          // 游戏名称
    const char* splash_title;       // 启动画面标题
    const char* splash_subtitle;    // 启动画面副标题
} GameMenuConfig;

// 初始化游戏菜单
void game_menu_init(const GameMenuConfig* config, const GameMenuCallbacks* callbacks);

// 清理游戏菜单
void game_menu_deinit(void);

// 处理按键
void game_menu_handle_key(KeyCode key);

// 渲染菜单
void game_menu_render(void);

// 强制渲染（不节流）
void game_menu_render_force(void);

// 切换到指定状态
void game_menu_switch_state(GameMenuState state);

// 获取当前状态
GameMenuState game_menu_get_state(void);

// 更新菜单（用于消息计时等）
void game_menu_update(void);

// 显示消息
void game_menu_show_message(const char* text, int duration_ms);

#endif // GAME_MENU_H

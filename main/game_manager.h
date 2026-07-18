#ifndef GAME_MANAGER_H
#define GAME_MANAGER_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "tft_display.h"

// 游戏类型枚举
typedef enum {
    GAME_TYPE_NONE = 0,
    GAME_TYPE_CHESS,      // 国际象棋
    GAME_TYPE_XIANGQI,    // 中国象棋
    GAME_TYPE_GOMOKU,     // 五子棋
    GAME_TYPE_GO9         // 9x9围棋
} GameType;

// 游戏结果枚举
typedef enum {
    GAME_RESULT_NONE,
    GAME_RESULT_WHITE_WIN,
    GAME_RESULT_BLACK_WIN,
    GAME_RESULT_DRAW
} GameResult;

// 游戏接口结构 - 所有棋种都需要实现这些函数
typedef struct GameInterface GameInterface;

struct GameInterface {
    GameType type;
    const char* name;

    // 初始化/重置游戏
    void (*init)(void* game);
    void (*reset)(void* game);

    // 移动相关 - 不同棋种可能有不同的移动方式
    bool (*select_piece)(void* game, int x, int y);  // 选择棋子（仅象棋类）
    bool (*make_move)(void* game, int x1, int y1, int x2, int y2);  // 移动棋子
    bool (*place_piece)(void* game, int x, int y);  // 直接落子（围棋/五子棋）

    // 验证
    bool (*is_valid_move)(void* game, int x1, int y1, int x2, int y2);
    bool (*can_select)(void* game, int x, int y);

    // 获取游戏状态
    int (*get_current_player)(void* game);  // 0=无,1=白/红,2=黑
    bool (*is_game_over)(void* game);
    GameResult (*get_result)(void* game);

    // 获取棋盘信息
    int (*get_board_width)(void);
    int (*get_board_height)(void);

    // 渲染
    void (*render)(void* game, TFT_Sprite* sprite, int offset_x, int offset_y,
                  int cursor_x, int cursor_y, int selected_x, int selected_y);

    // 获取游戏状态大小（用于蓝牙传输）
    size_t (*get_state_size)(void);
    void (*serialize_state)(void* game, uint8_t* buffer);
    void (*deserialize_state)(void* game, const uint8_t* buffer);

    // 游戏数据大小
    size_t game_data_size;
};

// 游戏管理器状态
typedef struct {
    GameType current_game;
    void* game_data;
    const GameInterface* game_interface;

    // UI状态
    int cursor_x;
    int cursor_y;
    int selected_x;
    int selected_y;
    bool has_selection;

    // 菜单状态
    int menu_index;
    bool in_menu;

} GameManager;

// 初始化游戏管理器
void game_manager_init(void);

// 切换游戏类型
void game_manager_switch_game(GameType type);

// 处理按键
void game_manager_handle_key(int key);

// 渲染当前屏幕
void game_manager_render(TFT_Sprite* sprite);

// 更新（用于动画等）
void game_manager_update(void);

// 获取游戏接口列表
const GameInterface** game_manager_get_game_list(int* count);

#endif // GAME_MANAGER_H

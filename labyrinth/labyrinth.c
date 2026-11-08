#include "labyrinth.h"
#include "../testkit/testkit.h"
#include <assert.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void printUsage() {
    printf("Usage:\n");
    printf("  labyrinth --map map.txt --player id\n");
    printf("  labyrinth -m map.txt -p id\n");
    printf("  labyrinth --map map.txt --player id --move direction\n");
    printf("  labyrinth --version\n");
}

bool isValidPlayer(char playerId) {
    // 有效 的玩家 ID 是 '0' 到 '9' 的数字
    return playerId >= '0' && playerId <= '9';
}

bool loadMap(Labyrinth *labyrinth, const char *filename) {
    // 从 map 文件中读取文件内容
    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        fprintf(stderr, "Error: Could not open map file %s\n", filename);
        return false;
    }
    // 根据文件内容填充 labyrinth 结构体
    char   line[MAX_COLS + 2]; // +2 for newline and null terminator
    int    row = 0;
    size_t col = -1;
    // 每次读取一行，检查行长度是否超过 MAX_COLS，并将其存储在 labyrinth->map 中
    while (fgets(line, sizeof(line), file) != NULL && row < MAX_ROWS) {
        // 去掉行尾的 '\n'（同时兼容 CRLF）
        size_t len = strcspn(line, "\r\n");
        // 检验列宽是否一致
        if (col == -1) {
            col = len;
        } else if (len != col) {
            fprintf(stderr, "Error: Inconsistent row lengths in map file\n");
            fclose(file);
            return false;
        }
        line[len] = '\0';
        // map[row] 是 char[MAX_COLS]，最多存 MAX_COLS - 1 个字符 + '\0'
        if (len >= MAX_COLS) {
            fprintf(stderr, "Error: Row %d exceeds maximum column limit\n", row);
            fclose(file);
            return false;
        }
        // 连同终止符一起拷贝，长度已校验，不会截断
        memcpy(labyrinth->map[row], line, len + 1);
        ++row;
    }
    labyrinth->rows = row;
    labyrinth->cols = (row > 0) ? strlen(line) : 0;
    fclose(file);
    return true;
}

Position findPlayer(Labyrinth *labyrinth, char playerId) {
    Position pos = {-1, -1};
    // 遍历迷宫地图，查找指定玩家 ID 的位置
    for (int i = 0; i < labyrinth->rows; ++i) {
        for (int j = 0; j < labyrinth->cols; ++j) {
            if (labyrinth->map[i][j] == playerId) {
                pos.row = i;
                pos.col = j;
                return pos;
            }
        }
    }
    return pos;
}

Position findFirstEmptySpace(Labyrinth *labyrinth) {
    Position pos = {-1, -1};
    // 行优先找到第一个空格
    for (int i = 0; i < labyrinth->rows; ++i) {
        for (int j = 0; j < labyrinth->cols; ++j) {
            if (isEmptySpace(labyrinth, i, j)) {
                pos.row = i;
                pos.col = j;
                return pos;
            }
        }
    }
    return pos;
}

bool isEmptySpace(Labyrinth *labyrinth, int row, int col) {
    // 检查当前位置是否是空格，并且在迷宫的边界内
    if (row < 0 || row >= labyrinth->rows || col < 0 || col >= labyrinth->cols) {
        return false;
    }
    // 因此检查当前位置是否为 '.'
    return labyrinth->map[row][col] == '.';
}

bool movePlayer(Labyrinth *labyrinth, char playerId, const char *direction) {
    // 先检查玩家 ID 是否有效
    if (!isValidPlayer(playerId)) {
        fprintf(stderr, "Error: Invalid player ID '%c'\n", playerId);
        return false;
    }
    // 查找玩家当前位置
    Position pos = findPlayer(labyrinth, playerId);
    if (pos.row == -1 || pos.col == -1) {
        // 如果没有找到玩家，默认放在第一个空格位置
        pos = findFirstEmptySpace(labyrinth);
        // 如果空格也没有找到，返回错误
        if (pos.row == -1 || pos.col == -1) {
            fprintf(stderr, "Error: No empty space available to place player '%c'\n", playerId);
            return false;
        }
    }
    // 根据方向计算新的位置
    int newRow = pos.row;
    int newCol = pos.col;
    if (strcmp(direction, "up") == 0) {
        newRow--;
    } else if (strcmp(direction, "down") == 0) {
        newRow++;
    } else if (strcmp(direction, "left") == 0) {
        newCol--;
    } else if (strcmp(direction, "right") == 0) {
        newCol++;
    } else {
        fprintf(stderr, "Error: Invalid direction '%s'\n", direction);
        return false;
    }
    // 检查新的位置是否在迷宫的边界内
    if (newRow < 0 || newRow >= labyrinth->rows || newCol < 0 || newCol >= labyrinth->cols) {
        fprintf(stderr, "Error: New position (%d, %d) is out of bounds\n", newRow, newCol);
        return false;
    }
    // 检查新的位置是否是空格
    // 如果不是空格，说明被占用，移动失败
    if (!isEmptySpace(labyrinth, newRow, newCol)) {
        fprintf(stderr, "Error: New position (%d, %d) is not an empty space\n", newRow, newCol);
        return false;
    }
    // 更新玩家位置
    labyrinth->map[pos.row][pos.col] = '.';      // 玩家离开原来的位置
    labyrinth->map[newRow][newCol]   = playerId; // 玩家移动到新的位置

    return true;
}

bool saveMap(Labyrinth *labyrinth, const char *filename) {
    // 将当前迷宫状态保存到指定的文件中
    FILE *file = fopen(filename, "w");
    if (file == NULL) {
        fprintf(stderr, "Error: Could not open file %s for writing\n", filename);
        return false;
    }
    for (int i = 0; i < labyrinth->rows; ++i) {
        fprintf(file, "%s\n", labyrinth->map[i]);
    }
    fclose(file);
    return true;
}

// Check if all empty spaces are connected using DFS
void dfs(Labyrinth *labyrinth, int row, int col, bool visited[MAX_ROWS][MAX_COLS]) {
    if (row < 0 || row >= labyrinth->rows || col < 0 || col >= labyrinth->cols) {
        return;
    }
    // 墙不能通过；玩家和空格都属于可通行区域
    if (visited[row][col] || labyrinth->map[row][col] == '#') {
        return;
    }
    visited[row][col] = true;
    // 递归访问上下左右的邻居
    dfs(labyrinth, row - 1, col, visited); // Up
    dfs(labyrinth, row + 1, col, visited); // Down
    dfs(labyrinth, row, col - 1, visited); // Left
    dfs(labyrinth, row, col + 1, visited); // Right
}

bool isConnected(Labyrinth *labyrinth) {
    // DFS 检查所有可通行格（空格和玩家）是否联通
    bool visited[MAX_ROWS][MAX_COLS] = {false};

    // 只从一个起点开始 DFS，不能对每个未访问格子都重新 DFS
    int startRow = -1;
    int startCol = -1;
    // 找到第一个可通行格作为起点
    for (int i = 0; i < labyrinth->rows && startRow == -1; ++i) {
        for (int j = 0; j < labyrinth->cols; ++j) {
            if (labyrinth->map[i][j] != '#') {
                startRow = i;
                startCol = j;
                break;
            }
        }
    }

    // 没有可通行格，视为连通
    if (startRow == -1) {
        return true;
    }

    dfs(labyrinth, startRow, startCol, visited);

    // 检查是否还有未访问的可通行格
    for (int i = 0; i < labyrinth->rows; ++i) {
        for (int j = 0; j < labyrinth->cols; ++j) {
            if (labyrinth->map[i][j] != '#' && !visited[i][j]) {
                return false;
            }
        }
    }
    return true;
}

bool isValidDirection(const char *direction) {
    return strcmp(direction, "up") == 0 || strcmp(direction, "down") == 0 ||
           strcmp(direction, "left") == 0 || strcmp(direction, "right") == 0;
}

void printMap(Labyrinth *labyrinth) {
    for (int i = 0; i < labyrinth->rows; ++i) {
        printf("%s\n", labyrinth->map[i]);
    }
}

// TestKit 会把真实 argv[0] 追加到测试用例的参数前，而测试数组本身
// 又以 "./labyrinth" 开头；忽略这个额外的程序名位置参数。
static int skipProgramPath(int argc, char *argv[], int first) {
    if (first < argc &&
        (strcmp(argv[first], "./labyrinth") == 0 || strcmp(argv[first], argv[0]) == 0)) {
        return first + 1;
    }
    return first;
}

int main(int argc, char *argv[]) {
    // 至少要有一个参数
    if (argc < 2) {
        printUsage();
        return 1;
    }
    // 解析命令行参数
    // 保存长选项的结构体数组
    static const struct option long_options[] = {{"map", required_argument, 0, 'm'},
                                                 {"player", required_argument, 0, 'p'},
                                                 {"move", required_argument, 0, 'd'},
                                                 {"version", no_argument, 0, 'v'},
                                                 {0, 0, 0, 0}};

    const char *mapFile       = NULL;
    const char *playerId      = NULL;
    const char *moveDirection = NULL;
    bool        version       = false;
    bool        bad_opt       = false;
    int         opt; // 用于 getopt_long 的返回值

    while ((opt = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
        switch (opt) {
        case 'm':
            // 处理 --map 参数
            mapFile = optarg;
            break;
        case 'p':
            // 处理 --player 参数
            playerId = optarg;
            break;
        case 'd':
            // 处理 --move 参数
            moveDirection = optarg;
            break;
        case 'v':
            // 处理 --version 参数
            version = true;
            break;
        default:
            bad_opt = true;
            break;
        }
    }
    int firstArg = skipProgramPath(argc, argv, optind);

    if (version) {
        if (bad_opt || mapFile || playerId || moveDirection || firstArg < argc) {
            fprintf(stderr, "Error: --version cannot be combined with other options\n");
            printUsage();
            return 1;
        }
        printf("%s\n", VERSION_INFO);
        return 0;
    }

    if (bad_opt) {
        fprintf(stderr, "Error: Invalid option\n");
        printUsage();
        return 1;
    }

    // 参数检查：忽略 TestKit 追加的程序名参数，只拒绝真正的多余参数
    if (firstArg < argc) {
        fprintf(stderr, "Error: Unexpected positional arguments\n");
        printUsage();
        return 1;
    }

    // 检查必需的参数
    if (!mapFile) {
        fprintf(stderr, "Error: Missing required --map option\n");
        return 1;
    }
    if (!playerId) {
        fprintf(stderr, "Error: Missing required --player option\n");
        return 1;
    }
    char playerChar = playerId[0];
    if (playerId[1] != '\0' || !isValidPlayer(playerChar)) {
        fprintf(stderr, "Error: Invalid player ID '%s'\n", playerId);
        return 1;
    }

    Labyrinth labyrinth = {0};
    if (!loadMap(&labyrinth, mapFile)) {
        return 1;
    }

    // 如果地图不连通，返回错误
    if (!isConnected(&labyrinth)) {
        fprintf(stderr, "Error: The map is not connected\n");
        return 1;
    }

    // 如果有移动方向，则尝试移动玩家
    if (moveDirection) {
        // 检查移动方向是否合法
        if (!isValidDirection(moveDirection)) {
            fprintf(stderr, "Error: Invalid move direction '%s'\n", moveDirection);
            return 1;
        }
        // 尝试移动玩家，如果失败则返回错误
        if (!movePlayer(&labyrinth, playerChar, moveDirection)) {
            return 1;
        }
        if (!saveMap(&labyrinth, mapFile)) {
            return 1;
        }
    } else {
        // 没有指定移动方向，则只显示当前地图
        printMap(&labyrinth);
    }

    return 0;
}

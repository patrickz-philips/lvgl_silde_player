# 图片资源静态编译说明

## 概述
为了将 `hair_dryer.png` 图片静态编译进可执行文件，需要将其转换为 C 数组格式。

## 转换方法

### 方法一：使用提供的 Python 脚本（最简单）

```bash
# 安装依赖（首次使用）
pip install pillow

# 运行转换脚本
python hair_dryer/assets/convert_image.py
```

脚本会自动：
- 读取 `hair_dryer.png`
- 转换为 LVGL 兼容的 C 数组格式
- 生成 `hair_dryer.c` 文件

### 方法二：使用 LVGL 在线图片转换工具

1. 访问 LVGL 在线转换工具：https://lvgl.io/tools/imageconverter
2. 上传 `hair_dryer.png` 文件
3. 配置选项：
   - **Name**: `hair_dryer`
   - **Color format**: `True color with alpha`
   - **Output format**: `C array`
4. 点击 "Convert" 按钮下载生成的 `hair_dryer.c` 文件
5. 将生成的文件放在 `hair_dryer/assets/` 目录下

### 方法三：使用 LVGL 命令行工具

如果已安装 Node.js：

```bash
# 安装工具
npm install -g @lvgl/lv_img_conv

# 转换图片
cd hair_dryer/assets
lv_img_conv hair_dryer.png -f true_color_alpha -o c_array > hair_dryer.c
```

## 生成的文件格式

转换后的 `hair_dryer.c` 文件应该包含类似以下内容：

```c
#include "lvgl/lvgl.h"

#ifndef LV_ATTRIBUTE_MEM_ALIGN
#define LV_ATTRIBUTE_MEM_ALIGN
#endif

const LV_ATTRIBUTE_MEM_ALIGN uint8_t hair_dryer_map[] = {
    // 图片数据...
};

const lv_img_dsc_t hair_dryer = {
    .header.always_zero = 0,
    .header.w = <宽度>,
    .header.h = <高度>,
    .data_size = sizeof(hair_dryer_map),
    .header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA,
    .data = hair_dryer_map,
};
```

## 在代码中使用

转换完成后，CMake 会自动检测并编译该文件。在 `hair_dryer.c` 中需要声明外部引用：

```c
#ifdef USE_STATIC_ASSETS
LV_IMG_DECLARE(hair_dryer);  // 声明静态图片
#endif
```

然后使用：

```c
#ifdef USE_STATIC_ASSETS
    lv_image_set_src(img, &hair_dryer);  // 使用静态编译的图片
#else
    lv_image_set_src(img, "A:/hair_dryer/assets/hair_dryer.png");  // 从文件系统加载
#endif
```

## 注意事项

1. 静态编译会增加可执行文件大小
2. 适合小型图片资源（< 500KB）
3. 大图片或大量图片建议继续使用文件系统加载
4. 转换后的 C 文件会在 Git 中跟踪，便于部署

## 编译模式策略

项目已配置为根据编译类型自动选择资源加载方式：

- **Release 模式**: 使用静态编译（需要 `hair_dryer.c` 文件）
  - 优点：单文件部署，加载速度快，无外部依赖
  - 适用场景：生产环境、最终发布版本

- **Debug 模式**: 使用动态加载（从文件系统读取 PNG）
  - 优点：可以随时修改图片无需重新编译
  - 适用场景：开发调试阶段

## 快速开始

### Windows 用户（推荐）

项目根目录下提供了便捷的批处理脚本：

**编译 Release 版本（静态编译）：**
```cmd
build_release.bat
```
此脚本会自动：
1. 检查并转换图片（如需要）
2. 配置 CMake Release 模式
3. 编译项目

**编译 Debug 版本（动态加载）：**
```cmd
build_debug.bat
```

### 手动编译命令

**Debug 版本（动态加载）：**
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug -DSELECTED_PROJECT=HAIR_DRYER
cmake --build build --config Debug
```

**Release 版本（静态编译）：**
```bash
# 1. 先转换图片（如果还没有 hair_dryer.c）
python hair_dryer/assets/convert_image.py

# 2. 配置并编译
cmake -B build -DCMAKE_BUILD_TYPE=Release -DSELECTED_PROJECT=HAIR_DRYER
cmake --build build --config Release
```

## 当前状态

- ✅ CMakeLists.txt 已配置支持 Debug/Release 模式切换
- ✅ hair_dryer.c 代码已支持条件编译
- ⏳ 需要生成 `hair_dryer.c` 文件用于 Release 编译


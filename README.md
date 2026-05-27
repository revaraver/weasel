# ReVar 输入法

基于：https://github.com/rime/weasel

ReVar 输入法是基于 Rime / 小狼毫（Weasel）的 Windows 输入法分支，目标是更适合中文编程、中文变量名和中英文混合输入场景。

本分支保留 Rime 的方案、词库、用户配置体系，同时增加 ReVar 自己的宿主策略、代码模式、候选窗位置控制和调试开关。

## 使用说明

### 1. 安装

适用系统：Windows 8.1 ~ Windows 11。

使用安装包安装：

```text
revar-input-0.17.4.7-installer.exe
```

安装后会注册独立的 ReVar 输入法，不复用小狼毫原 CLSID，也不覆盖用户的个人 Rime 配置。

用户配置目录仍然是：

```text
%APPDATA%\Rime
```

ReVar 专属配置文件：

```text
%APPDATA%\Rime\revar_input.yaml
```

### 2. 基本输入

安装后在 Windows 输入法列表中选择：

```text
ReVar 输入法
```

Rime 方案、词库和常规部署方式仍沿用 Rime / 小狼毫习惯。修改 Rime 配置或词库后，需要重新部署。

### 3. Ctrl+F10：ReVar 菜单

在任意宿主软件中按：

```text
Ctrl+F10
```

可以打开 ReVar 菜单。菜单会识别当前前台 EXE 的完整路径，并把策略写入：

```text
%APPDATA%\Rime\revar_input.yaml
```

主要功能：

- 当前 EXE 模式 / 替换策略
- 代码模式候选窗位置
- 当前 EXE 的候选窗 x/y/gap 数值调整
- 调试日志开关
- 清空调试日志
- 打开日志目录
- 清除当前 EXE 的 ReVar 记忆

### 4. 模式说明

默认模式是兼容模式：

```yaml
revar_input:
  default_mode: compatible
```

也就是默认尽量保持小狼毫式行为。

需要 ReVar 代码模式的宿主，建议用 Ctrl+F10 对当前 EXE 单独开启，而不是全局开启。

配置中主要看这些段落：

```yaml
revar_input:
  code_mode:
    enabled_exact_paths:
      - "c:\\path\\to\\app.exe"
    disabled_exact_paths: []

  host_policies:
    tsf_replace_exact_paths: []
    raw_unicode_exact_paths: []
    direct_replace_exact_paths: []
    backspace_unicode_exact_paths: []
```

路径匹配使用 normalized full path exact match，不用 contains 模糊匹配。

### 5. Godot / RVIR direct replace

对于已打补丁支持 RVIR 协议的 Godot，可把对应 EXE 加入：

```yaml
code_mode:
  enabled_exact_paths:
    - "d:\\git\\godot\\bin\\godot.windows.editor.x86_64.exe"

host_policies:
  direct_replace_exact_paths:
    - "d:\\git\\godot\\bin\\godot.windows.editor.x86_64.exe"
```

这一路线使用：

```text
raw: SendInput Unicode
候选/UI: async TSF edit session
commit: RVIR direct replace + expected_raw guarded replace
```

目标是降低输入阻尼，同时避免误删前文、吞字、乱序。

### 6. 调试日志

调试日志默认关闭：

```yaml
revar_input:
  debug:
    trace_enabled: false
```

关闭时不会写文件，也不会在热路径上做日志文件 IO。

需要诊断时可以通过 Ctrl+F10 菜单打开。日志位置：

```text
%TEMP%\revar_input_dev_trace.log
```

如果不需要调试，保持关闭即可。

### 7. 快捷键约定

```text
F1          detach_shadow_buffer
F2          预留 host mode 快切
Ctrl+F10    ReVar 菜单
Ctrl+F11    保留给 Rime switcher
```

### 8. 卸载说明

卸载程序只卸载 ReVar 程序和注册项，不应删除用户的个人 Rime 配置目录。

个人配置目录：

```text
%APPDATA%\Rime
```

如需彻底清理个人方案、词库、补丁，请手动备份后再处理。

## 上游与许可

上游项目：

- Rime：https://rime.im
- Weasel：https://github.com/rime/weasel
- librime：https://github.com/rime/librime

本分支基于 Weasel / Rime 生态开发，遵循原项目许可。详见仓库中的 LICENSE / COPYING 文件。

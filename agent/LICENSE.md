# Windows Agent 许可与对应源码

Copyright (C) 2026 Ultraterimal contributors

Ultraterimal Windows Agent 以及构建该程序所需的本项目对应源代码（`agent/`、`protocol/`、相关构建与测试脚本）采用 **GNU General Public License version 3 or later（GPL-3.0-or-later）**。你可以依照自由软件基金会发布的 GPL 第 3 版或任何后续版本重新分发和修改这些代码。程序不提供任何担保，包括适销性或特定用途适用性的默示担保。完整条款见随附的 `COPYING`。

x264 原有许可为 GPL-2.0-or-later；组合发行选择 GPL 第 3 版或后续版本，以兼容 OpenSSL、Abseil 等依赖的 Apache-2.0 许可。第三方代码保留其原始版权与许可，随安装包提供 `third_party_notices.txt`。本声明不重新许可第三方代码或前端模板，也不扩大到独立的 Server 业务代码。

每个公开分发的 Windows MSI 必须同时提供匹配版本的 `ultraterimal-agent-source-<version>.tar.gz`，与 MSI 放在同一下载目录，不要求收件人另购源码。源码包包括本项目提交、固定的 Ruvia 和 vcpkg 快照、依赖原始源码归档、许可、校验值及构建说明。不要只发布 MSI 而漏掉源码包。具体版本和源码位置记录在安装目录的 `source_info.txt`。

项目源代码及修改历史：<https://github.com/hyird/UltraTerminal>。

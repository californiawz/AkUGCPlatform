# Phase 0 技术风险验证

## 目标

证明同一份 UGC 源数据能够被 Desktop 和移动轻编辑器修改，并作为同一 Logic Pack 在 Win64、Android 和 Dedicated Server 中一致运行。

首个垂直切片：三波合作塔防。

详细实施顺序、当前基线、阶段任务和验收门禁见 [`ImplementationPlan.md`](ImplementationPlan.md)。

## 六周范围

| 周 | 交付 |
|---|---|
| 1 | UE 工程、构建矩阵、ADR、塔防切片定义 |
| 2 | Project Document、Prefab Schema、序列化和迁移 |
| 3 | Command System、Runtime Entity、Desktop 最小视口 |
| 4 | App 轻编辑原型、Trigger Graph、Logic IR |
| 5 | Lua 沙箱、Logic Pack、签名和 Android 加载 |
| 6 | 塔防切片、Win64/Android/DS 一致性与风险报告 |

## Go/No-Go 门禁

- UGC VM 无法访问 UObject 反射、文件系统和外部网络。
- 死循环和资源耗尽可被终止且不会导致宿主崩溃。
- 同一文档与输入在本地和 DS 产生一致的权威规则结果。
- UGC 包无法覆盖基础游戏资产或其他作品命名空间。
- Android 在目标设备预算内完成下载、加载、运行、卸载和作品切换。
- Desktop 与 App 的编辑操作产生相同的序列化 Command。

## 首期能力

- 官方 Prefab：地面、墙、基地、刷怪点、路径点、终点、塔位、敌人和基础塔。
- 编辑：放置、选择、移动、旋转、缩放、复制、删除和属性覆盖。
- 逻辑：游戏开始、波次开始、进入区域、死亡、计时器、伤害、生成、胜负和消息。
- 发布：Manifest、Scene Document、Logic IR、依赖、预算、哈希和签名。

## 明确不做

- 玩家上传资产、C++、插件和任意 Blueprint。
- 完整 Lua IDE、完整手机专业编辑器和复杂地形。
- 社区推荐、创作者现金结算和多人实时协作。
- 文生 3D、开放世界、完整 RPG/MMO。
- Kafka、MongoDB、Redis Cluster 和大规模 Kubernetes。

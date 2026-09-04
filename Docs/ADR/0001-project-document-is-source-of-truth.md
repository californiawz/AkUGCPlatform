# ADR-0001：UGC Project Document 是 Portable 项目的唯一真源

- 状态：Accepted
- 日期：2026-09-03

## 决策

Portable UGC 项目以版本化 `UGC Project Document` 为唯一真源。UE World、Actor 和 `.umap` 是编辑预览、缓存或发布编译产物，不作为跨 Desktop/App 的唯一源数据。

Document 至少描述 Project Manifest、Scene、Entity、PrefabId、Transform、Component Data、Logic Graph、Ruleset、Asset Dependency、Capability 和 Budget。

## 原因

- Desktop、App 和 AI 需要操作同一语义模型。
- UObject 和 `.umap` 无法安全、稳定地暴露给移动端编辑器。
- 版本化 Document 可支持迁移、Diff、协作、审计和重放。
- 发布编译器可以从 Document 生成不同平台的优化制品。

## 版本与迁移策略

- 缺少 `manifest.schemaVersion` 或显式版本为 `0` 的历史文档按 V0 处理。
- 当前支持从 V0 迁移到 V1；迁移会补齐项目版本，并将缺失或为零的 Component SchemaVersion 规范化为 1。
- 迁移不得生成缺失的 ProjectId、SceneId 或 EntityId；此类数据问题仍由 Validator 拒绝。
- 高于当前版本的文档必须在转换为 USTRUCT 前拒绝，避免未知字段被忽略后覆盖原文件。
- 迁移必须先完整验证，再统一修改；失败不得留下部分迁移结果。
- 保存只允许写出当前版本，并采用临时文件验证后原子替换目标文件。

## 影响

Desktop 视口中的任何可持久化编辑必须转化为 Command 并回写 Document。绕过 Command 直接修改预览 Actor 的变化不会被视为项目内容。

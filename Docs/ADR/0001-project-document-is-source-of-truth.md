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

## 影响

Desktop 视口中的任何可持久化编辑必须转化为 Command 并回写 Document。绕过 Command 直接修改预览 Actor 的变化不会被视为项目内容。

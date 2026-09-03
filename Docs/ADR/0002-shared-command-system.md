# ADR-0002：所有编辑入口共用 UGC Command System

- 状态：Accepted
- 日期：2026-09-03

## 决策

Desktop、Player App 和 AI 只能通过统一 Command 修改 Project Document。第一批命令包括 AddEntity、DeleteEntity、SetTransform、SetProperty、DuplicateEntity、ConnectLogicNode、DeleteLogicNode 和 SetRuleParameter。

每个 Command 必须可序列化、可校验、原子应用、可撤销、可重做并携带操作身份。

## 原因

- 避免 Desktop 维护 UObject 状态而 App 维护另一套 JSON 状态。
- 为撤销/重做、崩溃恢复、协作、AI Diff 和审计提供统一基础。
- 让客户端产生的操作在服务端重新验证，而不是信任最终快照。

## 影响

UI、Slate、UMG、AI 和网络层均不得成为 Document 的直接写入者。复杂操作使用多个原子 Command 组成 Transaction，失败时不得留下半完成状态。

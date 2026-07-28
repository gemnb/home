# Charles / 代理数据 回归测试清单

## 1. 实例与视图切换
- 打开“代理数据查看”页面，确认左侧实例列表可正常切换。
- 同一实例下切换 `代理数据` / `Charles` 子视图，不应出现空白页或脚本异常。
- 切换到另一实例后，上一实例的会话、过滤、高亮状态应正确收敛。

## 2. Raw 与 Charles 同源一致性
- 在同一实例产生新流量后，`代理数据` 中出现的原始包，在 `Charles` 中应能看到对应会话或 `Generic TCP / 未解析`。
- Raw 与 Charles 的总量感知应基本一致，不应出现明显“Raw 有 / Charles 无”的分叉。
- 切回旧实例时，不应混入其他实例的会话。

## 3. Charles 会话能力
- `Sequence` 视图可正常展示会话列表。
- `Structure` 视图按 `Host -> method/path(summary)` 分组正常。
- `Focus Host` 过滤可只保留目标 Host。
- `Method / Status / Content-Type` 过滤可组合使用。
- “重置过滤”可恢复所有过滤器默认值。

## 4. Charles 详情页
- `Overview` 显示连接ID、客户端IP、目标主机、SNI主机、目标端口等信息。
- `Request` / `Response` 可显示首行、Headers、Body 预览。
- `Headers` tab 只显示头信息。
- `Body` tab 只显示正文信息。
- `Hex` tab 仍可跳回原始包。
- `Timeline` 显示 Request / Latency / Response / Total。

## 5. 正文格式化
- JSON 正文应缩进格式化。
- XML 正文应缩进格式化。
- `application/x-www-form-urlencoded` 应显示为 `key = value` 形式。
- 普通文本应保持换行可读。

## 6. Raw 列表联动
- 在 Charles 中点击“切换到代理数据子视图”后，Raw 列表应滚动到首个关联包。
- 目标包应高亮。
- 高亮应可点击“清除高亮”取消。
- 高亮应在合理时间后自动收敛。

## 7. 详情按需加载
- 第一次打开会话详情时，若未缓存完整包，应出现“正在加载完整原始包”提示。
- 加载完成后应自动刷新对应 Request / Response / Headers / Body 内容。
- 加载失败时应优雅回退，不影响 Raw 视图使用。

## 8. 空态与边角
- 数据包记录关闭时，Raw 与 Charles 空态文案应明确。
- 无匹配过滤结果时，Charles 空态应清晰说明。
- 不应因切换标签页导致页面卡死或明显闪烁。

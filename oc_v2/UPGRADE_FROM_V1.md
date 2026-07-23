# 从V1交接包升级

旧目录主要描述单总线：

```text
opencode_handoff_stm32_onewire_20260723
```

新版目录：

```text
oc_v2
```

建议：

1. 保留旧目录作历史备份；
2. 把新版目录完整复制到工程根目录；
3. 新OpenCode对话只引用新版`00_READ_ME_FIRST.md`；
4. 不要把V1和V2内容混合后只读取部分文件；
5. OpenCode仍需核对本地真实源码。

# LNTD5.06(05)GW Rcent 表来源

固件 Step27 的 NTC 查表数据来自用户提供的 LTR `TS-AS-04-337` 规格书附录 `R-T data for LNTD5.06(05)G`。

- 使用列：`Rcent(kΩ)`；
- 温度范围：`-40~120°C`；
- 间隔：`1°C`；
- 固件存储单位：Ω；
- 表项数量：161；
- 插值：相邻表项之间整数线性插值。

AUTO 控制范围不是该表的一部分，仍在 `app_auto_control_config.h` 中独立定义为 `-25~60°C`。

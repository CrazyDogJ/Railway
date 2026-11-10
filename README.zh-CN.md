# Railway

Railway 在编辑器里构建铁路网。游戏中的列车从当前关卡的 `ARailwayDataActor` 读取样条、允许的行驶方向和站点。轨道 Actor 可以随 World Partition 流送，岔口和站点标记只用于编辑。

## 构建当前关卡

1. 放置 `ARailwayTrack` 并编辑样条。用 `ARailwayJunction` 连接开放轨道的起点或终点。要在主线中段设置道岔，将岔口的 `HostTrack` 和 `TrackDistance` 设为主线和对应里程，再将支线端点指向这个岔口。自由岔口可用轨道上的 **Snap Endpoints To Junctions** 对齐。
2. 放置 `ARailwayStationMarker`，指定宿主轨道和从样条首点起算的厘米数。也可以直接编辑轨道的 `Stations`。站名、停站时长和 `CustomData` 都会一并烘焙。
3. 在关卡编辑器中选择 **Build → Build Railway Data**。构建器会临时加载当前关卡的铁路源 Actor，创建或更新唯一的 Railway Data Actor。无法加载的来源、跨关卡引用或未对齐的端点会使构建失败，具体原因显示在通知和 Output Log 中。
4. **保存关卡及修改过的外部 Actor 包**。构建结果支持编辑器撤销。修改线路、站点或道岔后需要再次构建。

## 运行时接口

- 在 Railway Data Actor 的 **Network** 中查看 `Tracks`、`Edges`、`Stations` 的数组下标。轨道行保留构建时的编辑器标签。物理区间的两个行驶方向各有一个边索引。
- **Find Route Plan** 接收两个 `FRailwayTrackLocation`，每个位置由 `TrackIndex` 和原生样条里程 `TrackDistance`（cm）组成。起终点可以位于区间内部。**Find Route Plan To Station** 接收目标 `StationIndex`。勾选 **Stop At Intermediate Stations** 会把途经站点加入停站表。指定目的站始终是最后一站。
- 在列车的 **Route** 中选择 **Between Points**，再从下拉框选择起点和终点。轨道点按样条原生里程填写厘米数，站点直接使用烘焙位置。选择 **Ordered Tracks** 可逐段选择轨道和方向，入口与出口默认填 `-1`，由相邻轨道的合法岔口推导。若存在多个可用岔口，用入口或出口里程指定其中一个。
- **Compile Route Settings** 将一份配置转换为 `FRailwayPlan`，可在蓝图中检查错误、采样、或交给 **Start Prepared Route**。**Start Route** 和自动发车直接使用列车自己的 **Route**。已有程序仍可用 **Build Route Plan** 提交当前构建的有向边索引。早期手工填写的 `Route` 数组需按新格式配置一次。
- **Sample Track** 可直接从 Data Actor 采样世界姿态。列车的 **Sample Plan**、**Sample Route** 和客户端运动插值使用相同的数据。**Resolve Planned Stop** 根据站点索引读取烘焙定义。
- 列车优先使用显式指定的 `RailwayData`。未指定时，优先查找同关卡数据，再尝试世界中唯一的数据 Actor。多个独立铁路网同时加载时，在列车上明确指定所属数据 Actor。

## 图和生命周期

每条源样条保存一份 `FSplineCurves`、组件到世界的变换和默认上向量。只有端点、闭环接缝和岔口构成图的节点。每条有向边记录源轨道、起止节点、样条里程区间，以及在扁平后继数组中的范围。站点记录规范方向的边索引和样条里程，双向经过同一个站都能停靠。

`FGraphAStar` 以有向边为搜索状态，按实际经过的弧长计费。岔口根据一侧驶入、另一侧驶出的轨道切向生成后继表，排除同一区间的立即掉头和直角、钝角转向。查询起终点只占用临时搜索状态。增删站点不会改变图的分段。

Data Actor 没有根组件，不参与空间流送。关卡烘焙数据由服务端和客户端各自加载，网络只传列车计划与运动状态。轨道和场景站点使用源 Actor 的 GUID，轨道内嵌站点使用轨道 GUID 和持久行 GUID 组合寻址。首次构建后请保存分配了行 GUID 的轨道 Actor。名称仅用于显示，同名来源在下拉菜单中可区分。运行计划、A* 与列车采样使用当前构建的整数索引。

## 存档与回退

索引布局相同时，读档直接恢复已有计划。仅修改停站时长或自定义站点数据可沿用原构建标识。索引或布局变化时，新存档按轨道、岔口和站点身份重建有向区间，恢复原有的经过顺序、重复圈次、停站游标和行驶进度。停站中的列车使用新站点时长扣除已停留的时间。新增的途经站不会自动插入已经保存的时刻表。

列车的 **Recovery Point** 可以选站点，也可以选轨道并填写里程。无法完整恢复时，列车清零速度和驾驶输入，在该点进入 Idle。未配置或该点已删除时使用当前网络第一条轨道的起点。没有可用铁路网时返回失败并保留列车现场。蓝图可读取 **Last Restore Result**，区分直接恢复、重映射、回退停放和失败。

旧的纯索引存档可以使用原构建，升级时也会尝试保留未改变且可辨认的索引布局。如果布局已经变更且存档没有稳定引用，会使用回退点。

# Railway

Railway 在编辑器里构建铁路网。游戏中的列车从当前关卡的 `ARailwayDataActor` 读取样条、允许的行驶方向和站点。轨道 Actor 可以随 World Partition 流送，岔口和站点标记只用于编辑。

## 构建当前关卡

1. 放置 `ARailwayTrack` 并编辑样条。用 `ARailwayJunction` 连接开放轨道的起点或终点。要在主线中段设置道岔，将岔口的 `HostTrack` 和 `TrackDistance` 设为主线和对应里程，再将支线端点指向这个岔口。自由岔口可用轨道上的 **Snap Endpoints To Junctions** 对齐。
2. 放置 `ARailwayStationMarker`，指定宿主轨道和从样条首点起算的厘米数。也可以直接编辑轨道的 `Stations`。站名、停站时长和 `CustomData` 都会一并烘焙。
3. 在关卡编辑器中选择 **Build → Build Railway Data**。构建器会临时加载当前关卡的铁路源 Actor，创建或更新唯一的 Railway Data Actor。无法加载的来源、跨关卡引用或未对齐的端点会使构建失败，具体原因显示在通知和 Output Log 中。
4. **保存关卡及修改过的外部 Actor 包**。构建结果支持编辑器撤销。修改线路、站点或道岔后需要再次构建。

## 沿轨放置装饰物

放置 **Railway Track Decoration**（`ARailwayTrackDecoration`），给 **Decoration Mesh** 选择网格，或用蓝图子类组合多个组件。选择 **Host Track**，用 **Track Distance** 指定沿轨里程。**Placement Offset** 使用轨道局部坐标，X 向前、Y 向右、Z 向上，例如站牌放在右侧两米可将 Y 设为 200 cm。

蓝图可重写 **On Track Aligned**，C++ 可重写非 const 的 `OnTrackAligned_Implementation(const FTransform& TrackTransform)`。调用时 Actor 已完成对齐并应用偏移，可以直接调整 Actor 的位置、旋转或缩放，例如用地面检测结果修改支撑柱底部高度。**Refresh Decoration Placement** 可在地形变化后手动重新执行。**Build Railway Data** 也会刷新当前关卡的装饰物。保存修改过的 Actor 包后，游戏通过 World Partition 加载最终摆放结果。

## 运行时接口

- 在 Railway Data Actor 的 **Network** 中查看 `Tracks`、`Edges`、`Stations` 的数组下标。轨道行保留构建时的编辑器标签。物理区间的两个行驶方向各有一个边索引。
- **Find Route Plan** 接收两个 `FRailwayTrackLocation`，每个位置由 `TrackIndex` 和原生样条里程 `TrackDistance`（cm）组成。起终点可以位于区间内部。**Find Route Plan To Station** 接收目标 `StationIndex`。勾选 **Stop At Intermediate Stations** 会把途经站点加入停站表。指定目的站始终是最后一站。
- 在列车的 **Route** 中选择 **Between Points**，再从下拉框选择起点和终点。轨道点按样条原生里程填写厘米数，站点直接使用烘焙位置。选择 **Ordered Tracks** 可逐段选择轨道和方向，入口与出口默认填 `-1`，由相邻轨道的合法岔口推导。若存在多个可用岔口，用入口或出口里程指定其中一个。
- **Compile Route Settings** 将一份配置转换为 `FRailwayPlan`，可在蓝图中检查错误、采样、或交给 **Start Prepared Route**。**Start Route** 和自动发车直接使用列车自己的 **Route**。已有程序仍可用 **Build Route Plan** 提交当前构建的有向边索引。早期手工填写的 `Route` 数组需按新格式配置一次。
- **Sample Track** 可直接从 Data Actor 采样世界姿态。列车的 **Sample Plan**、**Sample Route** 和客户端运动插值使用相同的数据。**Resolve Planned Stop** 根据站点索引读取烘焙定义。
- 列车优先使用显式指定的 `RailwayData`。未指定时，优先查找同关卡数据，再尝试世界中唯一的数据 Actor。多个独立铁路网同时加载时，在列车上明确指定所属数据 Actor。

## 玩家调度面板

在本地 Player Controller 的蓝图里调用 **Create Widget**，Class 选 **Railway Plan Widget**，Owning Player 传入当前控制器，Train 传入要查看的列车。然后调用 **Add to Player Screen**。要切换列车，复用同一个控件并调用 **Set Train**。面板也可以嵌入已有 UMG 菜单。全屏交互时由打开菜单的游戏逻辑设置 **Game and UI** 输入模式和鼠标光标，关闭时恢复原输入模式。

面板显示速度、驾驶模式、当前轨道和原生里程、目的站或终点轨道里程、剩余距离、下一站、停站倒计时及各段运行方向。悬停终点可查看世界坐标。每次停靠单独一行，实际到站标记为「已停靠」，从行程中段发车时起点之前的站次标记为「已越过」。旧存档若未记录实际到站标记，其游标之前的站次也按「已越过」显示。

- **取消停靠** 删除一个尚未服务的站次。勾选 **显示可加停站** 可补回当前路线中仍可到达的站次。同名站和不同行程里程的重复停靠分别处理。
- **直达** 保留已服务站次和正在进行的停站，之后只服务所选目的站。取消该直达目的站后，行程终点恢复为几何路线末端。
- **改设终点** 从列车所在位置寻路。可选站点，或选择轨道并输入以米为单位的原生里程，也可勾选沿途停站。无合法路线时面板显示原因。列车待命或行程结束时，还可以勾选 **指定新起点发车**，自由组合起点和终点并激活新行程。
- **取消路线并停车** 在当前轨道点立即停稳，保留可存档、可同步的位置。**从预设起点发车** 使用列车的 Route 配置，重新定位到预设起点并发车。
- 联机时，在游戏的 **Player Controller** 蓝图中添加 **Railway Plan Control** 组件，并让 GameMode 使用这个控制器。远端玩家通过自己的控制器发送请求；服务端按所见计划版本和站次重新判定，可在列车蓝图重写 **Can Player Edit Plan** 限制调度权限。默认允许本世界的玩家调度。未安装组件的远端玩家仍可查看面板。

编辑器视口或 PIE 控制台输入 `railway.Debug.Trains 1` 显示列车概览，`2` 展开轨道、停站、驾驶输入、速度限制、计划与快照版本、网络角色和铁路网构建信息，`0` 关闭。选中列车时只显示当前视口世界中被选中的列车。使用实时视口并开启 **On Screen Debug** 显示标志。

自动化用例位于 RailwayEditor 的 `Private/Tests` 目录，可在 Automation 面板运行 `Railway.Dispatch`。

### 用普通 Widget Blueprint 制作自己的面板

新建父类为 **User Widget** 的 Widget Blueprint，在 Designer 中自行放置文本、列表、下拉框和按钮。添加 `ARailwayTrain` 类型变量 **Train**，勾选 **Expose on Spawn**。也可以继承 **Railway Plan Widget** 并在 Class Defaults 勾选 **Use Designer Layout**，沿用其 Train 变量、**Set Train** 和 **On Displayed Train Changed** 事件。

1. 在 **Event Construct** 中绑定列车的 **On Railway Timetable Changed** 并主动读取一次。换车时解除旧列车的绑定，在 **Event Destruct** 中解除当前绑定并清理定时器。
2. **Get Railway Plan Overview** 返回同步状态、当前轨道、速度（km/h）、行程进度、下一站、停留时间，以及实际终点的轨道里程和世界姿态。等 Readiness 为 **Ready** 后保存 **View**。可每 0.2 秒更新连续数值。所有距离均为厘米。
3. **Get Railway Timetable** 输出 Scheduled 和 Addable 两组站次。勾选 **Include Addable** 可读沿途可补回的站次。每一行持有当前版本、站点身份、行程里程和访问状态。把整行保存在自己的行控件中，按 **Status** 显示已越过、已停靠、正在停靠、下一站或待停靠。**Get Railway Route Sections** 返回到实际终点为止的有向区间。时刻表变化时重读列表；展开可加停列表时也可重新读取。
4. 行按钮把 **Get Owning Player** 和保存的那一行直接接入 **Skip Railway Visit**、**Add Railway Visit** 或 **Go Direct to Railway Visit**。全局按钮把玩家和 Overview 的 View 接入 **Activate Railway Preset** 或 **Cancel Railway Route**。
5. 打开选择器时用 **Get Railway Route Sources** 获取轻量选项。站点直接使用选项的 **Point**，轨道在 Point 上填 **Track Distance**。把终点接入 **Set Railway Destination** 可从当前位置改线；待命时用 **Start Railway Between Points** 提交起终点。两个节点均可选择沿途停站。

以上七个操作都是 **Event Graph** 异步节点，自带 **Accepted**、**Failed** 和 **Message** 引脚。单机及本地权威玩家直接执行，远端玩家通过自己 Player Controller 上的 **Railway Plan Control** 组件提交。**Accepted** 表示服务端已应用修改，客户端在时刻表变化事件中刷新显示。**Can Send Railway Plan Command** 可检查本地通道和版本，最终权限由列车的 **Can Player Edit Plan** 决定。使用按钮创建时的行或 View，可防止过期列表误改另一站次。回执超时会提示结果未知，应重新读取列车状态。

## 图和生命周期

每条源样条保存一份 `FSplineCurves`、组件到世界的变换和默认上向量。只有端点、闭环接缝和岔口构成图的节点。每条有向边记录源轨道、起止节点、样条里程区间，以及在扁平后继数组中的范围。站点记录规范方向的边索引和样条里程，双向经过同一个站都能停靠。

`FGraphAStar` 以有向边为搜索状态，按实际经过的弧长计费。岔口根据一侧驶入、另一侧驶出的轨道切向生成后继表，排除同一区间的立即掉头和直角、钝角转向。查询起终点只占用临时搜索状态。增删站点不会改变图的分段。

Data Actor 没有根组件，不参与空间流送。关卡烘焙数据由服务端和客户端各自加载，网络只传列车计划与运动状态。轨道和场景站点使用源 Actor 的 GUID，轨道内嵌站点使用轨道 GUID 和持久行 GUID 组合寻址。首次构建后请保存分配了行 GUID 的轨道 Actor。名称仅用于显示，同名来源在下拉菜单中可区分。运行计划、A* 与列车采样使用当前构建的整数索引。

## 存档与回退

索引布局相同时，读档直接恢复已有计划。仅修改停站时长或自定义站点数据可沿用原构建标识。索引或布局变化时，新存档按轨道、岔口和站点身份重建有向区间，恢复原有的经过顺序、重复圈次、停站游标和行驶进度。停站中的列车使用新站点时长扣除已停留的时间。新增的途经站不会自动插入已经保存的时刻表。

列车的 **Recovery Point** 可以选站点，也可以选轨道并填写里程。无法完整恢复时，列车清零速度和驾驶输入，在该点进入 Idle。未配置或该点已删除时使用当前网络第一条轨道的起点。没有可用铁路网时返回失败并保留列车现场。蓝图可读取 **Last Restore Result**，区分直接恢复、重映射、回退停放和失败。

旧的纯索引存档可以使用原构建，升级时也会尝试保留未改变且可辨认的索引布局。如果布局已经变更且存档没有稳定引用，会使用回退点。

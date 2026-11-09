# Railway 0.2 · 轨道线路编辑

包内包含 C++ 源码与 UE 5.8.4 Win64 编辑器模块。使用共享插件目录时直接更新已有 Railway；使用项目插件时，将 Railway 放在项目的 Plugins 目录。重启编辑器后，在 Place Actors 面板搜索 Railway。每个工程只启用一个 Railway 副本。

## 从一条主轨拉出支线

1. 放置两个 **Railway Track**，分别命名为 Main 和 Spur。用 Spline 控制点编辑形状。示例中 Main 从 (0,0,100) 通到 (2000,0,100)，Spur 的远端放在 (1800,800,100)。轨道 Actor 保持单位缩放。
2. 将 **Railway Junction** 放在 Main 中段附近，在 Details 中用 **Host Track** 吸管选 Main。节点吸到最近的轨道位置，并成为 Main 的子 Actor。用平移工具沿线拖动黄色箭头，或用 **Track Position** 输入精确位置。0 表示首点，1 表示末点，0.5 表示弧长中点。
3. 将 Spur 的 **Start Junction** 指向该节点。Spur 的首端随节点移动，Main 的原始曲线保持连续。可将多条支线接到同一节点。需要平顺进岔时，用 Spline 切线调整接头方向。开启视口 **Realtime** 可持续看到跨 Actor 的联动更新。
4. 将 **Railway Station** 放在 Spur 附近，设置 **Host Track = Spur**。拖动青色箭头选停车点，设置 **Station Name** 和 **Dwell Seconds**。视口文字显示站名，Actor 原点就是车体的停车基准。改变轨道长度或形状时，站点按已保存的弧长比例跟随。
5. 放置 **Railway Train**，在 **Route** 中依次添加 Main 和 Spur，两项 **Reverse** 均关闭。第一项设定后，编辑器会把车体附在 Main 的出发端，调整轨道后车体随之更新。启动 Simulate 后，列车在中途岔口进入 Spur，停靠站点后继续开到终点。

## 验证不同走向

- **直行**。Route 只填 Main。列车穿过岔口，在主轨末端停车。
- **反向汇入**。Route 填 Spur、Main，两项都勾选 Reverse。车体预览切换到支线远端并调转朝向，运行时沿 Spur 逆行，经岔口汇入 Main 并开往 Main 首端。
- **挪站或换线**。选 Station，直接拖动或换 Host Track。选择新轨道时从当前位置投影到最近点。轨道折返或自交附近可以用 Track Position 精确选定。取消 Host Track 可解除附着。
- **运行规则**。Start Route 在发车时读取行程与当前已加载的站点，跨段计算制动距离，只停靠实际经过的轨道区间。运行中改动线路后，重新调用 Start Route 会从起点重建并发车。行程末端停车；有些站停时间为零时，仍会触发到站事件。

## 闭合轨道

选中 Railway Track 的 Spline 组件，在 Details 勾选 **Closed Loop**，引擎会把最后一个控制点连回第一个控制点。列车 Route 只填这条轨道时，从第一个点出发，跑完一圈后停车。勾选 **Reverse** 可反向绕行。连续填写同一条闭合轨道并保持相同方向，可指定多圈行程，圈间按站点配置停靠。

支线进出环线时，将 Junction 的 **Host Track** 设为环线，再把开放支线的 **Start Junction** 或 **End Junction** 指向它。闭合轨道自己的 Start Junction 和 End Junction 不参与连接。在“支线 A → 环线 → 支线 B”行程中，列车按环线项的方向，从入口岔口驶至出口岔口，可跨越首尾接缝。两个岔口位于同一里程时会绕行整圈。仅停靠实际经过的站点；整圈起终点上的站在返回时停靠一次。

## 旧关卡与蓝图

现有自由 Junction 和端点连接可继续使用 **Snap Endpoints To Junctions**。旧版站点仍保存在 Track 的高级属性 **Legacy Stations** 中，原有行程继续停靠。若要把旧站改成可拖动 Actor，创建 Railway Station、指定 Host Track，然后复制旧名称、Position 和停留时间，最后移除对应的旧数组项，以免重复停靠。

蓝图可以读取 **Speed**、**State** 和 **Current Station**，并绑定 **On Station Arrival**。Cruise Speed 的单位为 cm/s，Acceleration 与 Braking 为 cm/s²，Response Time 的单位为秒。路线错误时在 Output Log 筛选 LogRailway 查看行程项编号及原因。轨道可以使用开放或闭合 Spline。可在 Session Frontend → Automation 中运行 Railway.Route 下的两项回归测试。

## 蓝图扩展接口

- **Start Route** 继续按 Route 从起点发车，并对齐有宿主的支线端点。**Build Route Plan** 只读取当前轨道几何和已加载站点，将传入的 Legs 转为 Sections 与 Stops。调用失败时读取 Out Error。运行或预览前需先完成岔口吸附。
- **Start Prepared Route** 从 Start Distance（整条行程的累计厘米数）静止发车；起点之前的停站跳过，恰好位于起点的停站仍会触发。自建计划需自行选择合法岔口和停站；此节点检查区间、站距和段间空间连续性。失败的计划不会覆盖正在运行的行程。Plan 使用场景 Actor 引用，持久化时由游戏提供跨加载的标识。
- **Sample Plan** 可在发车前采样已生成的计划。**Sample Route** 采样运行中的计划，返回段序号、从 Spline 首点起算的局部厘米数、车体世界姿态；布尔值表示是否有效。Section 中的 Entry Distance 是本轨道里程，Travel Distance 是沿指定方向的正长度，计划停站 Distance 是行程累计里程。编辑器预览可在列车蓝图中覆写 **Get Preview Location**，输出带方向的轨道和局部里程。该函数在编辑器会频繁调用，适合使用缓存。
- 蓝图可读取 Active Plan、Distance、Leg、Next Stop（从零起算）、Wait（秒）、Has Sent Pre Leave 和 Get Route Length。运动参数支持运行时修改；**External Speed Limit** 为独立限速输入（cm/s，负值解除），通过现有加减速模型生效。到站、预离站和离站委托沿用原接口。
- 锚点蓝图可调用 **Set Track Anchor** 指定 Host Track 与 0–1 的弧长比例，传空轨道解除附着。**Project To Host** 从当前位置更新比例，**Refresh Anchor** 把该比例应用到 Actor。

## 联机运动与存档

服务端驱动 Step。ActivePlan 变化时复制线路；TrainMoveState 的 RepNotify 将带服务器时间的累计里程样本送入客户端缓冲区。客户端按计划版本配对、在两份样本之间沿轨道插值。Snapshot Rate Hz 默认 15，Interpolation Delay 默认 0.15 秒（列车蓝图的 Railway → Networking）。客户端中 Distance、Leg 和 Speed 对应当前显示位置，权威到站和离站事件仍由服务端驱动。远端需要已加载、可解析且几何一致的轨道。读档和重开发车使用新版本号，等待新计划和样本就绪后直接定位。

Get Save Game Data 保存实际执行的计划及累计里程、运行状态和停站游标。行驶时保存速度，停站时保存剩余时间和预离站通知是否已发。Leg 和 Current Station 在加载时推导；同里程多个停站仍通过 NextStop 区分。旧的按属性名序列化的存档缺少预离站标记，若在提示之后存档，恢复时可能再次提示。自定义计划中的 Track 引用必须能在加载时解析到原关卡中的轨道。

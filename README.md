# cli-elevator

用 UACMe 的 **RAiLaunchAdminProcess + DebugObject** 方法（`ucmDebugObjectMethod`）把任意命令行以管理员权限
跑起来，同时把目标程序的 stdout/stderr 回流到当前终端、把当前终端的键盘输入转发给目标程序。

产出两个文件：`bin\cli-elevator.x64.exe`、`bin\cli-elevator.x86.exe`。

## 构建

```
build.bat          :: 两个架构都编
build.bat clean    :: 清掉 build\ 和 bin\
```

需要 Visual C++ 工具链。脚本自己用 `vswhere` 找 VS 并调用 `vcvarsall.bat`；如果你已经在
Developer Command Prompt 里，直接用当前环境。

## 用法

程序接受两种参数形态，看第一个 token 是不是 `\\.\pipe\...` 决定走哪个模式。

### a. 原始命令行（给用户用）

```
cli-elevator cmd
cli-elevator cmd.exe /c whoami /groups
cli-elevator "C:\Program Files\Git\bin\bash.exe" -i
```

后面那一坨会被**原样**当成目标程序的命令行（不解析、不转义），工作目录取命令行里那个程序自己
所在的目录。

### b. `\\.\pipe\xxxxx`（broker，内部用）

```
cli-elevator \\.\pipe\cli-elevator-1234-abcdef
```

broker 会去连 `<base>-stdin`、`<base>-stdout`、`<base>-stderr` 三条命名管道。这是入口提升自己时
用的形态，一般不需要手敲。

## 运行时序

入口（a）：

0. 先建一个 job object（`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`）并把自己也放进去；
1. 生成管道名 `\\.\pipe\cli-elevator-<pid>-<随机>`，创建三条命名管道，**入口是服务端**；
2. 用 `ucmDebugObjectMethod` 提升并启动自己（broker），只把 b 形式的管道名传过去；
3. 等 broker 连上三条管道，同时盯着 broker 进程 —— UAC 被拒 / 提升失败就直接报错退出；
4. 通过 `-stdin` 发命令行：`DWORD 命令行字节数` + `DWORD 入口当前目录字节数` + 两个 NUL 结尾的 UTF-16 串
   （当前目录只用来解析相对路径的程序名）；
5. 开三个线程：目标 stdout → 本终端 stdout，目标 stderr → 本终端 stderr，本终端键盘输入 → `-stdin`；
6. 等 broker 退出，然后**用 broker 的退出码退出**；broker 又是用目标程序的退出码退出的，
   所以 `cli-elevator cmd.exe /c exit 42` 最后返回 42。

第 0 步的 job 是安全网：broker 在第 2 步才创建，继承进这个 job，目标进程再继承 broker。入口不管是正常
退出、被任务管理器结束、终端窗口被关还是自己崩了，最后一个 job 句柄都会在进程消失时关闭，内核随即把
broker、目标进程、以及目标自己拉起来的进程一起终止。job 句柄故意不显式关闭 —— 让进程退出顺手关掉它，
那一下才是触发点。

broker（b）：

1. `ShowWindow(GetConsoleWindow(), SW_HIDE)` **隐藏自己的控制台**——隐藏不是关闭：目标程序要共用这个
   控制台，Ctrl+C 也靠它投递；
2. 连三条管道；
3. 读命令行，把第一个 token 解析成程序路径，取**它所在目录**作为工作目录，建匿名管道启动目标程序：

   ```
   CREATE_NEW_PROCESS_GROUP | CREATE_UNICODE_ENVIRONMENT
   STARTF_USESTDHANDLES -> 三个匿名管道
   不传 CREATE_NEW_CONSOLE
   ```

   不新建控制台是有意的：目标挂在 broker 那个隐藏控制台上，控制事件才投得过去（启动瞬间
   `GetConsoleProcessList` 还看不到它，进程要在控制台上登记之后才算数）；
4. 转发目标的 stdout/stderr 回上游，转发上游 stdin 给目标 stdin（`0x03` 除外，见下）；
5. 目标退出后把管道里剩下的输出排空，用目标的退出码退出。

## Ctrl+C

- 入口装 `SetConsoleCtrlHandler`，收到 `CTRL_C_EVENT` / `CTRL_BREAK_EVENT` 时往 `-stdin` 写一个
  `0x03` 字节，然后返回 TRUE 把自己那一下吞掉（入口不能死，它要等 broker）；
- broker 在 stdin 流里读到 `0x03`，就把这一下 Ctrl+C 交给目标所在的控制台：

  1. `GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)`。组 ID 必须是 0：CTRL_C_EVENT **不支持**定向到某个
     进程组，传 PID 会「返回成功但没人收到」；
  2. 再往 `CONIN$` 写一条 Ctrl+C 按键记录（`WriteConsoleInput`），也就是用户真按 Ctrl+C 时控制台输入
     缓冲里出现的那条记录，防备某些控制台宿主把第 1 步吞掉；
  3. 只有第 1 步返回失败时，才补一发 `CTRL_BREAK_EVENT`（Ctrl+Break 是另一种信号，能救回那些把
     Ctrl+C 关掉的程序，但对「本来打算优雅处理 Ctrl+C」的程序就是直接打死，所以不做无条件兜底）。

目标进程和 broker 共用同一个（隐藏的）控制台（`GetConsoleProcessList` 里能看到它），所以 1、2 两步对它
都成立；broker 自己也在那个控制台上，用 `SetConsoleCtrlHandler` 把自己的那一份吞掉。万一目标跑在别的
控制台上，broker 会先 `FreeConsole` + `AttachConsole(目标pid)` 再发。

**不用 `CREATE_NEW_PROCESS_GROUP`**：那个标志会把目标进程组的 Ctrl+C 关掉（微软文档明确写了），而且定向
到进程组还要指望控制台登记过这个组 —— 目标进程的 std 全是管道、从头到尾不碰控制台，很可能压根没登记，
于是 `GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid)` 就「返回成功但没人收到」。现在目标待在控制台的
默认进程组里，收到的是货真价实的 Ctrl+C。

因为发的是整个控制台的 Ctrl+C，实际表现和真实控制台一致：

- 提示符上按 Ctrl+C：cmd 打印 `^C` 回到提示符，**不会退出**（真实控制台也这样），要退就敲 `exit`；
- 前台跑着的命令（`ping` 之类）会被正常中断；
- 入口这一侧：控制台在按 Ctrl+C 时会取消挂起的 `ReadFile`（`ERROR_OPERATION_ABORTED`），
  转发线程必须继续听下去 —— 把它当成 EOF 就会导致「按一次 Ctrl+C 之后键盘再也转发不过去」。

## 环境变量

| 变量 | 作用 |
| --- | --- |
| `CLI_ELEVATOR_TRACE=1` | 打开 trace |
| `CLI_ELEVATOR_TRACE_FILE=<路径>` | trace 同时写到文件（broker 的控制台是隐藏的，只有文件看得见它） |
| `CLI_ELEVATOR_NO_ELEVATE=1` | 测试用：不提升，入口直接把 broker 起来，单独测管道协议 |

## 文件

| 文件 | 内容 |
| --- | --- |
| `src/main.c` | 入口、模式分发、入口模式（a） |
| `src/broker.c` | broker 模式（b） |
| `src/uacme.c` | UACMe 的 `ucmDebugObjectMethod` 及其依赖：`AicLaunchAdminProcess`、`supCreateBindingHandle`、`supGetProcessDebugObject`、`ucmxCreateProcessFromParent` |
| `src/util.c` | 小工具（管道读写、token 切分、trace） |
| `src/appinfo/{x64,x86-32}` | UACMe 里 MIDL 生成的 AppInfo RPC 客户端 stub，原样拷贝 |
| `LICENSE.UACME.md` | UACMe 的 BSD-2-Clause 许可 |

## 已知限制

- 目标程序**没有自己的可见控制台**（共用 broker 的隐藏控制台），它的一切输出都靠管道回到原终端。
- 交互式程序从管道读输入。cmd 收到管道输入时会把命令行再回显一遍，所以同一行会看到两次：一次是你
  打字的本地回显，一次是 cmd 的回显。这是管道 stdin 的固有结果，不是重复转发。
- 只转发 Ctrl+C / Ctrl+Break；关窗口之类的信号不处理。
- 需要 UAC 不是"始终通知"级别，且当前用户属于 Administrators 组，否则 AppInfo 会拒绝。
- 目标程序的命令行总长上限 64 KB（管道 framing 的 `CLI_MAX_CMDLINE`）。
- **每跑一次会在进程列表里留一个 `ComputerDefaults.exe` 空壳，要手动结束**：它是提权用的牺牲品，由
  AppInfo 用 `DEBUG_PROCESS` 建出来并一直握着那个 debug object，于是它永远停在"等调试事件"上（0 句柄、
  0 CPU、一个挂起线程）。`TerminateProcess` 返回成功、退出码也变成 0，但内核不回收它的进程对象。
  试过并且不行的办法：`NtRemoveProcessDebug` 脱钩（被拒，AppInfo 给的句柄没有 debug 权限）、把 debug
  事件排空（没用，还每次多花 1.6 秒）、把它塞进 job（`IsProcessInJob` 说在里面，job 关闭时照样收不走它）。
  这是这套 UACMe 手法本身的产物，不是入口清理漏做。

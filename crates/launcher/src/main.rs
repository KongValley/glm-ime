#![windows_subsystem = "windows"]
// 常驻后台程序：禁止控制台窗口（诊断走 %LOCALAPPDATA%\glm-ime\logs\launcher.log）
//! glm-ime launcher —— 命名管道 ↔ 引擎 stdio 桥（M1，tokio 版）。
//!
//! 模型：每连接一个引擎进程（engine-rs 冷启动 4.4ms，进程即会话，天然隔离）。
//! 职责：单实例守卫 / 命名管道多实例接入 / 双向行中继（select 双向）/ 引擎崩溃自动重生（重放 init）。
//! 协议：引擎侧见 proto/engine.v0.md；管道侧行协议与引擎 stdio 同构（一请求一行）。
//! 诊断只走 stderr。

use std::os::windows::io::AsRawHandle;
use std::os::windows::process::CommandExt;
use windows_sys::Win32::System::Pipes::GetNamedPipeClientProcessId;
use std::process::Stdio;
use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::windows::named_pipe::ServerOptions;
use tokio::process::{Child, ChildStdin, ChildStdout, Command};
use windows_sys::Win32::Foundation::{CloseHandle, ERROR_ALREADY_EXISTS, HANDLE, INVALID_HANDLE_VALUE};

mod acl;

const MAX_ENGINE_RESPAWNS: usize = 5;

/// 追加式文件日志：%LOCALAPPDATA%\\glm-ime\\logs\\launcher.log
fn llog(msg: &str) {
    let mut dir = match std::env::var("LOCALAPPDATA") {
        Ok(d) => d,
        Err(_) => return,
    };
    dir.push_str("\\glm-ime\\logs");
    let _ = std::fs::create_dir_all(&dir);
    let path = format!("{dir}\\launcher.log");
    if let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(&path) {
        use std::io::Write;
        let _ = writeln!(f, "{msg}");
    }
}

/// 追加式文件日志：%LOCALAPPDATA%\glm-ime\logs\launcher.log


struct EngineSlot {
    child: Child,
    stdin: ChildStdin,
    stdout: BufReader<ChildStdout>,
}

/// 引擎命令：GLM_ENGINE_CMD（空格分词）优先，否则 exe 同目录 glm-engine-rs.exe。
fn engine_command() -> Vec<String> {
    if let Ok(s) = std::env::var("GLM_ENGINE_CMD") {
        let v: Vec<String> = s.split_whitespace().map(str::to_string).collect();
        if !v.is_empty() {
            return v;
        }
    }
    let exe = std::env::current_exe().expect("current_exe");
    let sibling = exe.parent().unwrap().join("glm-engine-rs.exe");
    vec![sibling.to_string_lossy().into_owned()]
}

async fn spawn_engine(cmd: &[String]) -> std::io::Result<EngineSlot> {
    let mut child = Command::new(&cmd[0])
        .args(&cmd[1..])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .creation_flags(0x08000000) // CREATE_NO_WINDOW：引擎不得弹控制台窗口
        .spawn()?;
    let stdin = child.stdin.take().expect("stdin piped");
    let stdout = child.stdout.take().expect("stdout piped");
    let pid = child.id().unwrap_or(0);
    eprintln!("[launcher] engine pid={pid} spawned");
    Ok(EngineSlot { child, stdin, stdout: BufReader::new(stdout) })
}

async fn send_line(w: &mut ChildStdin, line: &str) -> std::io::Result<()> {
    w.write_all(line.as_bytes()).await?;
    w.write_all(b"\n").await?;
    w.flush().await
}

async fn read_line_from<R: tokio::io::AsyncBufRead + Unpin>(rd: &mut R) -> Option<String> {
    let mut line = String::new();
    match rd.read_line(&mut line).await {
        Ok(0) | Err(_) => None,
        Ok(_) => Some(line.trim_end_matches(['\n', '\r']).to_string()),
    }
}

/// 处理一个管道客户端：中继双向；引擎崩溃则重生并重放 init（会话状态归零）。
/// 重生收敛在 respawn() 单一入口：EOF 与 write-failed 两条触发路径最终都走它，
/// 临界区内原子完成 kill→spawn→init 重放→吃 init 回复，避免双路径覆盖竞争。
async fn serve_client(pipe: tokio::net::windows::named_pipe::NamedPipeServer, cmd: Arc<Vec<String>>) {
    // 会话日志标识：客户端进程 pid（全事件日志，便于事后追溯）
    let client_pid: u32 = unsafe {
        let mut pid: u32 = 0;
        let ok = GetNamedPipeClientProcessId(pipe.as_raw_handle() as _, &mut pid);
        if ok != 0 { pid } else { 0 }
    };
    let session_t0 = std::time::Instant::now();
    llog(&format!("session start client_pid={client_pid}"));
    let mut req_count: u64 = 0;
    let (mut pipe_rd, mut pipe_wr) = tokio::io::split(pipe);
    let mut pipe_rd = tokio::io::BufReader::with_capacity(1 << 16, pipe_rd);

    // 握手行（原样转发；engine.v0 或 PIME 兼容 {"method":"init",...}）
    let Some(init_line) = read_line_from(&mut pipe_rd).await else {
        eprintln!("[launcher] handshake read failed/EOF");
        return;
    };

    let mut engine = match spawn_engine(&cmd).await {
        Ok(e) => e,
        Err(e) => {
            eprintln!("[launcher] engine spawn failed: {e}");
            return;
        }
    };
    if send_line(&mut engine.stdin, &init_line).await.is_err() {
        return;
    }
    // 注意：初次握手【不吃】init 回复——select 的 stdout 分支会把它转发给客户端（行数守恒）。
    // 只有重生路径才需要吃掉重生 init 的回复（客户端视角多出的一行）。

    // 未回请求队列（FIFO：引擎一请求一响应）。引擎崩溃/卡死时重放，客户端无感。
    let mut pending: std::collections::VecDeque<String> = std::collections::VecDeque::new();
    let engine_timeout = std::time::Duration::from_millis(2000);

    // 重生引擎并重放未回请求。返回 false = 放弃会话。
    async fn respawn_and_replay(
        engine: &mut EngineSlot, cmd: &[String], init_line: &str,
        pending: &std::collections::VecDeque<String>, respawns: &mut usize, reason: &str,
    ) -> bool {
        if *respawns >= MAX_ENGINE_RESPAWNS { return false; }
        *respawns += 1;
        eprintln!("[launcher] respawn #{} reason={reason} pending={}", *respawns, pending.len());
        llog(&format!("respawn #{} reason={reason} pending={}", *respawns, pending.len()));
        engine.child.kill().await.ok();
        *engine = match spawn_engine(cmd).await { Ok(e) => e, Err(e) => { eprintln!("[launcher] respawn failed: {e}"); return false; } };
        if send_line(&mut engine.stdin, init_line).await.is_err() { return false; }
        let _ = read_line_from(&mut engine.stdout).await; // 吃 init 回复（行数守恒）
        for line in pending {
            if send_line(&mut engine.stdin, line).await.is_err() { return false; }
        }
        true
    }

    let mut respawns = 0usize;
    loop {
        tokio::select! {
            line = read_line_from(&mut pipe_rd) => {
                let Some(line) = line else { break }; // 客户端断开
                // 竞态防护：引擎退出后其 stdin 管道缓冲仍接受写入（假成功），必须先查进程状态
                let dead = matches!(engine.child.try_wait(), Ok(Some(_)));
                if dead || send_line(&mut engine.stdin, &line).await.is_err() {
                    if !respawn_and_replay(&mut engine, &cmd, &init_line, &pending, &mut respawns, "pipe-write").await { break; }
                    if send_line(&mut engine.stdin, &line).await.is_err() { break; }
                }
                req_count += 1;
                pending.push_back(line);
            }
            out = async {
                if pending.is_empty() {
                    // 空闲期：引擎本就不输出，不可施加超时（否则误判卡死反复重生）
                    Ok(read_line_from(&mut engine.stdout).await)
                } else {
                    // 有待回请求：2s 无响应视为卡死
                    match tokio::time::timeout(engine_timeout, read_line_from(&mut engine.stdout)).await {
                        Ok(v) => Ok(v),
                        Err(_) => { llog("engine response timeout (2s) pending>0"); Err(()) }
                    }
                }
            } => {
                let out: Option<String> = match out {
                    Err(()) => {
                        if !respawn_and_replay(&mut engine, &cmd, &init_line, &pending, &mut respawns, "timeout").await { break; }
                        continue;
                    }
                    Ok(v) => v,
                };
                match out {
                    Some(out) => {
                        pending.pop_front();   // 一请求一响应：出队已回请求
                        if pipe_wr.write_all(out.as_bytes()).await.is_err()
                            || pipe_wr.write_all(b"\n").await.is_err() { break; }
                        let _ = pipe_wr.flush().await;
                    }
                    None => {
                        if !respawn_and_replay(&mut engine, &cmd, &init_line, &pending, &mut respawns, "eof").await { break; }
                    }
                }
            }
        }
    }
    let killed = engine.child.kill().await.is_ok();
    let _ = tokio::time::timeout(std::time::Duration::from_secs(2), engine.child.wait()).await; // 回收，防残留
    llog(&format!(
        "session end client_pid={client_pid} requests={req_count} respawns={respawns} kill={} dur={:?}",
        if killed { "ok" } else { "already-exited" },
        session_t0.elapsed()
    ));
    eprintln!("[launcher] client disconnected (respawns={respawns})");
}

/// 启动时清理孤儿引擎进程（上次异常退出可能遗留；用户曾见任务栏一堆引擎窗口——防复发）。
/// 单实例守护保证此刻无其它 launcher 运行，故所有同名引擎均为孤儿，安全清理。
fn kill_orphan_engines() {
    let _ = std::process::Command::new("taskkill")
        .args(["/IM", "glm-engine-rs.exe", "/F"])
        .creation_flags(0x08000000) // CREATE_NO_WINDOW
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status();
}

fn single_instance() -> Option<HANDLE> {
    let name: Vec<u16> = "Global\\glm-ime-launcher-singleton"
        .encode_utf16().chain(std::iter::once(0)).collect();
    let h = unsafe { windows_sys::Win32::System::Threading::CreateMutexW(std::ptr::null(), 0, name.as_ptr()) };
    if h == 0 || h == INVALID_HANDLE_VALUE {
        return None;
    }
    if unsafe { windows_sys::Win32::Foundation::GetLastError() } == ERROR_ALREADY_EXISTS {
        unsafe { CloseHandle(h) };
        return None;
    }
    Some(h)
}

fn username() -> String {
    let mut buf = [0u16; 256];
    let mut len = buf.len() as u32;
    let ok = unsafe { windows_sys::Win32::System::WindowsProgramming::GetUserNameW(buf.as_mut_ptr(), &mut len) };
    if ok == 0 {
        return "default".into();
    }
    String::from_utf16_lossy(&buf[..(len - 1) as usize])
}

#[tokio::main]
async fn main() {
    kill_orphan_engines(); // 防孤儿引擎堆积
    let Some(_mutex) = single_instance() else {
        eprintln!("[launcher] another instance already running");
        std::process::exit(1);
    };

    let cmd: Arc<Vec<String>> = Arc::new({
        let v = engine_command();
        eprintln!("[launcher] engine command: {v:?}");
        v
    });

    let pipe_name = format!(r"\\.\pipe\{u}\glm-ime\launcher", u = username());
    eprintln!("[launcher] listening on {pipe_name}");

    let sec: Arc<acl::PipeSecurity> = Arc::new(acl::PipeSecurity::new());
    let sa_ptr: *mut std::ffi::c_void = &sec.sa as *const _ as *mut std::ffi::c_void;

    let mut server = match unsafe { ServerOptions::new().create_with_security_attributes_raw(&pipe_name, sa_ptr) } {
        Ok(s) => s,
        Err(e) => {
            eprintln!("[launcher] create pipe failed: {e:?}");
            std::process::exit(1);
        }
    };
    loop {
        if let Err(e) = server.connect().await {
            // 关键修复：未连接的实例绝不交给会话（否则 read 永久阻塞、泄漏实例，
            // 累积至 FIFO/PIPE 上限后所有新连接报 ERROR_PIPE_BUSY=231，launcher 假死）
            eprintln!("[launcher] connect error: {e:?}; recreating instance");
            llog(&format!("connect error: {e:?}; recreating"));
            server = match unsafe { ServerOptions::new().create_with_security_attributes_raw(&pipe_name, sa_ptr) } {
                Ok(s) => s,
                Err(e) => {
                    eprintln!("[launcher] recreate failed: {e:?}");
                    llog(&format!("recreate failed: {e:?}"));
                    tokio::time::sleep(std::time::Duration::from_millis(500)).await;
                    continue;
                }
            };
            continue;
        }
        let client = server;
        let cmd = Arc::clone(&cmd);
        let sa_ptr = sa_ptr;
        tokio::spawn(async move { serve_client(client, cmd).await });
        server = match unsafe { ServerOptions::new().create_with_security_attributes_raw(&pipe_name, sa_ptr) } {
            Ok(s) => s,
            Err(e) => {
                eprintln!("[launcher] create pipe failed: {e:?}");
                std::process::exit(1);
            }
        };
    }
}

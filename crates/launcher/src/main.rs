//! glm-ime launcher —— 命名管道 ↔ 引擎 stdio 桥（M1，tokio 版）。
//!
//! 模型：每连接一个引擎进程（engine-rs 冷启动 4.4ms，进程即会话，天然隔离）。
//! 职责：单实例守卫 / 命名管道多实例接入 / 双向行中继（select 双向）/ 引擎崩溃自动重生（重放 init）。
//! 协议：引擎侧见 proto/engine.v0.md；管道侧行协议与引擎 stdio 同构（一请求一行）。
//! 诊断只走 stderr。

use std::process::Stdio;
use std::sync::Arc;
use tokio::io::{AsyncBufReadExt, AsyncWriteExt, BufReader};
use tokio::net::windows::named_pipe::ServerOptions;
use tokio::process::{Child, ChildStdin, ChildStdout, Command};
use windows_sys::Win32::Foundation::{CloseHandle, ERROR_ALREADY_EXISTS, HANDLE, INVALID_HANDLE_VALUE};

mod acl;

const MAX_ENGINE_RESPAWNS: usize = 5;

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
    let (mut pipe_rd, mut pipe_wr) = tokio::io::split(pipe);
    let mut pipe_rd = tokio::io::BufReader::with_capacity(1 << 16, pipe_rd);

    // 握手行（原样转发；engine.v0 或 PIME 兼容 {"method":"init",...}）
    let Some(init_line) = read_line_from(&mut pipe_rd).await else {
        eprintln!("[launcher] handshake read failed/EOF");
        return;
    };
    eprintln!("[launcher] handshake: {init_line}");

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

    let mut respawns = 0usize;
    loop {
        tokio::select! {
            line = read_line_from(&mut pipe_rd) => {
                let Some(line) = line else { eprintln!("[launcher] pipe EOF"); break }; // 客户端断开
                eprintln!("[launcher] recv<-client: {line}");
                // 竞态防护：引擎退出后其 stdin 管道缓冲仍接受写入（假成功），必须先查进程状态
                let dead = matches!(engine.child.try_wait(), Ok(Some(_)));
                if dead || send_line(&mut engine.stdin, &line).await.is_err() {
                    eprintln!("[launcher] send failed/dead={} respawning ({respawns}) line={line}", dead);
                    if respawns >= MAX_ENGINE_RESPAWNS { break; }
                    respawns += 1;
                    engine.child.kill().await.ok();
                    engine = match spawn_engine(&cmd).await { Ok(e) => e, Err(e) => { eprintln!("[launcher] respawn spawn failed: {e}"); break } };
                    if send_line(&mut engine.stdin, &init_line).await.is_err() { eprintln!("[launcher] respawn init send failed"); break; }
                    let _ = read_line_from(&mut engine.stdout).await; // 吃掉重生 init 回复，行数守恒
                    if send_line(&mut engine.stdin, &line).await.is_err() { break; }
                }
            }
            out = read_line_from(&mut engine.stdout) => {
                match out {
                    Some(out) => {
                        eprintln!("[launcher] fwd->client: {out}");
                        if pipe_wr.write_all(out.as_bytes()).await.is_err()
                            || pipe_wr.write_all(b"\n").await.is_err() { eprintln!("[launcher] pipe write failed"); break; }
                        let _ = pipe_wr.flush().await;
                    }
                    None => {
                        // 引擎 EOF：重生并重放 init（会话状态归零，见 docs/m1-design.md）
                        if respawns >= MAX_ENGINE_RESPAWNS { break; }
                        respawns += 1;
                        eprintln!("[launcher] engine died, respawning ({respawns})");
                        engine.child.kill().await.ok();
                        engine = match spawn_engine(&cmd).await { Ok(e) => e, Err(e) => { eprintln!("[launcher] respawn spawn failed: {e}"); break } };
                        if send_line(&mut engine.stdin, &init_line).await.is_err() { eprintln!("[launcher] respawn init send failed"); break; }
                        let _ = read_line_from(&mut engine.stdout).await;
                    }
                }
            }
        }
    }
    engine.child.kill().await.ok();
    eprintln!("[launcher] client disconnected (respawns={respawns})");
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
            eprintln!("[launcher] connect error: {e:?}");
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
